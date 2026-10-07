// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <AudioGenerator.h>

#include <cstddef>
#include <cstdint>
#include <optional>

#include "DecoderArena.h"
#include "OggOpus.h"
#include "PassClock.h"
#include "audio/SourceReader.h"

struct OpusDecoder;  // libopus's state (opaque; "libopus/include/opus.h" in the .cpp)

// An Ogg Opus track as an ESP8266Audio generator (docs/OPUS.md): the
// lib/core reader (OggOpus: the headers, the packets, the trims) drives
// ESP8266Audio's bundled libopus, the fixed-point decoder-only copy it
// compiles anyway, through "libopus/include/opus.h" alone. Its own
// AudioGeneratorOpus is never included or linked: it overruns its packet
// buffer on ordinary tagged files, drops 60-120 ms packets, applies half
// the pre-skip and no end trim (the research report, section 3.3).
//
// The decode: every packet is split into its frames (oggopus::splitPacket)
// and each frame decoded as a code-0 packet of its own, one opus_decode()
// call per frame (at most 20 ms of CELT or hybrid, 60 ms of SILK: 2,880
// samples), with the same output, bit for bit, as the whole packet in one
// call (opus_decode_native() loops over the frames the same way; checked on
// real files by tools/opus_check). So one decode call is never ~35 ms (a
// 120 ms packet), and the PCM buffer stays at one frame. A frame that
// libopus refuses is concealed for its duration (opus_decode(nullptr)), and
// so is a packet whose TOC reads but whose framing doesn't (libopus would
// refuse it too): the count runs on as the file says; a packet with no
// TOC to size it by (0 bytes, an impossible count) is dropped and the
// page's granule puts the count right at the page's end. The
// Timeline says which of the decoded samples are audio: the pre-skip is
// dropped from the top, the EOS page's granule trims the end, so the track
// is sample-exact from the file alone and TrimFeed has nothing to do
// (armed {0,0}: no lead sample is ever handed over). A damaged or lost
// page leaves a gap the reader sizes from the next page's granule
// (Timeline::gap()): it is filled before the next packet is decoded, two
// frames of concealment (libopus's PLC, the last frame's size) then
// silence, a frame's worth a loop() step, so the count never slips and the
// track keeps its exact length (a join's sample count holds); a gap over
// 10 s (oggopus::kMaxGapSamples) ends the track early instead, logged. The
// output is always
// 48 kHz stereo: a mono file is decoded by a 1-channel decoder and
// expanded L = R in place; a family-1 file's mapping table is applied here
// (L = decoded map[0], R = decoded map[1]: the channels swapped, or one of
// them on both sides). The header's output gain goes to libopus
// (OPUS_SET_GAIN, applied in fixed point, free at 0); on the speaker, which
// sums L + R, phase inversion is disabled (OPUS_SET_PHASE_INVERSION_DISABLED;
// decided at the open, by the output then). The open refuses what can't
// play, with a sentence for the track's note: surround (more than 2
// channels: the bundled libopus has no multistream decoder), frames under
// 10 ms (2.5 ms frames decoded at 1.0x realtime on the Core2), Vorbis and
// the rest (lib/core/OggOpus: Reader::refusal()).
//
// The frames go to the output a frame at a time, ConsumeSamples(): with
// RingOutput that is RingFeed's budgeted block write (the converter's block
// path, ~2.4 % of a core cheaper than a frame at a time at 48 kHz), with
// the bench's CountingOutput the base class's frame loop. When the output
// refuses a sample (the ring full, the pass's budget spent) the pass ends
// and the rest of that frame is offered again next loop(), as ESP8266Audio's
// generators do; Timeline::decoded() is called once per frame.
//
// How long a pass is (docs/OPUS.md gate G6: at most 30 ms, FLAC's own
// figure at a start on this build, so the UI loop on the same core keeps
// its turn; 20 ms at M0, section 8.11 for the change): the pass ends on
// time, not on frames alone. Every step (decodeNext(): a page read or a
// slice of one, a decode call, a fill) is timed (lib/core PassClock), and
// the next one is taken only while the time the pass has used plus what
// the last step took stays within kPassBudgetUs (15 ms), so a pass is
// ~15 ms plus the output's conversion unless one step is longer than that
// by itself. The first step of every pass runs whatever the last step
// took: a step over the budget (an SD stall; a page turn at 160 MHz near
// the budget) ends the pass it is in, and the next pass goes on, one step
// at a time while that lasts. (M2's first cut asked the rule before the
// first step too, so one long step stopped every pass after it before it
// stepped, for good: the ring drained and the track never ended.) The
// steps are kept short: a decode call is one frame (6-9 ms at 240 MHz, 11
// at 160); the reader reads the audio pages in slices of kPageSlice (8 KB,
// ~4-5 ms of SD time: the M0 gate's 22-79 ms passes were whole-page reads,
// 16 KB a second of audio at 128k and 64 KB at 510k, on top of two decode
// calls), each slice one reader step that says Pending (lib/core/OggOpus:
// Reader::setReadSlice()), and one more Pending once the page is whole
// and checked, so its first frame's decode is a step of its own (M3's
// device check measured the last slice, the CRC and the decode together
// at 19-29 ms on every file, the step over the budget that F2dmg found;
// section 10); and after a damaged page it looks for the next
// good one a 4 KB chunk a step the same way, the page it finds a slice a
// step (ogg::PageReader::findStep(); M2 read that scan and the page in one
// step, 55 and 139 ms on the device's damaged copies: docs/OPUS.md 8.11).
// The output's budget bounds a pass only through what reaches it, so the
// pass also ends on its own once it has decoded a frame's worth of samples
// (kPassSamples: kept or dropped), made kPassCalls decode calls, dropped
// kPassMalformed packets, or the reader said Pending (a slice read, or its
// own share of pages that yield no packet for one call: 64 pages or 128 KB
// of reads): a pre-skip of 3,840 (four 20 ms frames thrown away before the
// first kept one), a stretch of junk packets, or thousands of empty pages
// can't hold the decode task for more than a few decode calls or page
// reads per pass. The next pass carries on from where it was.
//
// Memory: the page buffer (64 KB), the packet buffer (60 KB) and the
// frame scratch (1,276 B) are PSRAM, allocated at the first open and kept;
// the reader (~500 B), the timeline and the frame table (~200 B) are
// members (internal RAM: the object stays under the 4 KB threshold). The
// decoder's state (opus_decoder_get_size(2), ~26.5 KB) and the frame's PCM
// (11,520 B) are a layout of the backend's DecoderArena, the pinned block
// in the fast lower half of the PSRAM shared with libmad (one decoder at a
// time; docs/RESAMPLER.md section 10d), or, for M0's A/B (the console's O
// knob, gate G1), a block malloc'd per track in internal RAM or in the
// PSRAM above 0x3FA00000. libopus keeps its scratch on the decode task's
// stack (VAR_ARRAYS: the M0 gate measured 13,000 B used of the task's
// stack at most, the fuzz file included; Core2AudioBackend's kDecodeStack).
//
// The backend opens the track first (open(): the headers and the tail
// scan, ~5 reads and ~33 KB on an mStream 128k file since M4 (the tail's
// 16 KB window checked in memory, the first audio page read whole and
// kept in hand for the first pass; docs/OPUS.md section 10): the rate for
// a join's continuity and the exact length before any frame), then
// begin()s it; a begin() on a file not opened opens it. With a record of
// an earlier open of the same file (the backend's OpusOpenCache, by the
// path and the size: a seek on the playing track, a track played before,
// the resume point at a boot) the open is one read, the BOS page's header
// checked against the record (oggopus::Reader::openFrom()); record() gives
// the backend what a fresh open learnt. The decode task only. A start part of the way in (docs/OPUS.md
// section 9; lib/core/OggOpus's class comment is the plan) is the reader's
// plan, made between the open and the begin (planStartMs() for a seek or a
// resume by its second, the tail rule first; planStart() for a resume
// anchor's sample: a bisection by page headers to the last page before
// the target less the preroll, 200 ms for a seek (oggopus::kSeekPrerollMs:
// the smallest that keeps the 128k files at 35 dB at their worst start,
// section 10) and 600 ms for a resume point), then begin() by it
// (setStartPlan()): the reader at
// the plan's page (in hand already when the plan's check read it: no
// read), the timeline by the plan, so the packets before the preroll are
// skipped by their TOCs undecoded (`skipped`), the preroll is decoded and
// dropped, and the first sample kept is the one asked, as a FLAC's
// (exact). The decoder is initialised afresh at every begin()
// (opus_decoder_init(): the reset a start part of the way in needs, and
// more). The first kept sample's trimmed index is recorded (landedSample())
// and the end line says it beside the target, so the log proves a landing.
class OpusGenerator : public AudioGenerator {
public:
  // The arena layout's parts.
  static constexpr size_t kStatePart = 0;
  static constexpr size_t kPcmPart = 1;
  static constexpr size_t kPcmBytes = oggopus::kMaxFrameSamples * 2 * sizeof(int16_t);  // 11,520
  // A pass's own caps (the class comment): samples decoded, kept or
  // dropped (one 60 ms SILK frame, three 20 ms CELT ones: ~20 ms of
  // decoding at the research's 6-7 ms a call); decode calls (ten 2.5 ms
  // frames are 1,200 samples, more than the output's 1,114-sample budget
  // at 48 kHz, so a normal pass never meets it); malformed packets dropped
  // (each is a reader step, a page read at most).
  static constexpr uint32_t kPassSamples = oggopus::kMaxFrameSamples;
  static constexpr uint32_t kPassCalls = 10;
  static constexpr uint32_t kPassMalformed = 16;
  // A pass's time (the class comment; PassClock): after its first step, the
  // next is taken only while the pass's time so far plus the last step's
  // stays within this.
  static constexpr uint32_t kPassBudgetUs = 15000;
  // The audio pages are read in slices of this many bytes, a reader step
  // each (Reader::setReadSlice()).
  static constexpr uint32_t kPageSlice = 8192;
  // A gap's fill starts with this many concealed frames (the research's
  // 6.9: libopus's PLC fades its last frame out over about two), then zeros.
  static constexpr uint32_t kFillConceal = 2;
  // The decoder's state for 2 channels (a mono file's is smaller and fits).
  static size_t stateBytes();
  // The layout's sizes: {stateBytes(), kPcmBytes}.
  static void layout(size_t sizes[2]);

  // Where the state and the PCM go (M0's A/B; the arena's block otherwise).
  enum class Placement : uint8_t { Low, Internal, High };
  static const char* placementName(Placement p);

  // `arena`: the backend's; `layout`: this generator's layout in it (-1:
  // none: malloc'd per track).
  OpusGenerator(DecoderArena& arena, int layout);
  ~OpusGenerator() override;

  // The track: the file open in `file` (the backend's), its headers read
  // and its length found, or, with `hint` (the backend's cache's record of
  // an earlier open of this path at this size), from the record after one
  // read checks it (the class comment; a record the check refuses is
  // followed by the full open). `speaker`: the output at the open is the
  // speaker (phase inversion off). False: refusal() says why ("Ogg Vorbis
  // isn't supported (only Opus)", "surround Opus (6 channels) isn't
  // supported", "damaged Opus header", ...), for the log and the track's
  // note, and refusalNote() the same in a few words for Now Playing's toast
  // ("surround Opus isn't supported"; oggopus::Reader::refusalNote()).
  // Logs a line on success.
  bool open(AudioFileSource* file, bool speaker, const oggopus::OpenRecord* hint = nullptr);
  bool opened() const { return opened_; }
  // The last open() took the hint (one read), not the file's headers.
  bool openedFromRecord() const { return fromRecord_; }
  // After open(): what it learnt, for the cache (false: not open, or the
  // length isn't known: no last page found).
  bool record(oggopus::OpenRecord* out) const { return reader_ && opened_ && reader_->record(out); }
  const char* refusal() const { return refusal_; }
  const char* refusalNote() const { return note_; }
  // After open(): the exact trimmed length (0: the tail scan found no page).
  uint64_t lengthSamples() const { return reader_ ? reader_->lengthSamples() : 0; }
  uint32_t lengthMs() const { return reader_ ? reader_->lengthMs() : 0; }
  const oggopus::Head& head() const { return reader_->head(); }

  // For the next begin(): where the decoder's state goes.
  void setPlacement(Placement p) { placement_ = p; }
  Placement placement() const { return placement_; }

  // ---- a start part of the way in (the class comment) ----
  // After open(): the plan for a start at `ms`, the tail rule first
  // (Reader::planStartMs(): the last 5 s and past the end start at 0:00),
  // with `prerollMs` of decoding before the target (oggopus::kSeekPrerollMs
  // for a seek, kResumePrerollMs for a resume point), by header probes
  // (2-8 of them and ~20-140 KB of reads on an mStream 128k file, ~80 KB
  // the mean; up to 450 KB on 64 KB pages: docs/OPUS.md 9.3; planUs() how
  // long it took). Returns the ms it lands at (0: from the top). `out->fromTop`
  // with a target above 0 (inside the first page or the first preroll) is
  // still a start part of the way in: from the top, the samples before the
  // target dropped. The backend logs the plan.
  uint32_t planStartMs(uint32_t ms, uint32_t prerollMs, oggopus::StartPlan* out);
  // The same for a trimmed sample (a resume anchor's: oggopus::checkAnchor()
  // applied the tail rule to it already), `prerollSamples` before it.
  void planStart(uint64_t sample, uint32_t prerollSamples, oggopus::StartPlan* out);
  uint32_t planUs() const { return planUs_; }
  // The next begin() starts by `plan` (null: from the top, as before). A
  // plan is a start part of the way in whatever its `fromTop` says (that is
  // where reading starts). open() forgets it (a new track).
  void setStartPlan(const oggopus::StartPlan* plan);
  // After begin(): the trimmed sample asked (0: from the top), and the
  // first kept sample's trimmed index (-1: nothing kept yet): the end line
  // says both (logEnd()), the log's proof that a start landed where asked.
  uint64_t startTarget() const { return planned_ ? plan_.target : 0; }
  int64_t landedSample() const { return landedT_; }

  // ---- AudioGenerator ----
  // The decoder on its state, the reader at the first audio packet (or at
  // the plan's page: setStartPlan()), the output told 48 kHz stereo.
  // `source` must be the file open()ed (else it is opened here, as the
  // speaker's).
  bool begin(AudioFileSource* source, AudioOutput* output) override;
  // One pass: frames to the output until it refuses one, the pass's time
  // is up (kPassBudgetUs), its own caps are met (kPassSamples, kPassCalls,
  // kPassMalformed), or the track's end (false). Safe to call after the
  // end (false).
  bool loop() override;
  // The state given back (the arena's block, or the malloc'd one). Safe to
  // repeat. The counters stay for logEnd().
  bool stop() override;
  bool isRunning() override { return running; }

  // ---- after the end ----
  // The track ended before its audio did: the file cut short (no EOS page),
  // another stream after ours, an EOS page promising more samples than it
  // held, or a gap too long to fill. endText() says which (or that the
  // track hasn't ended: the bench stops after 20 s of it).
  bool endedEarly() const;
  const char* endText() const;
  // The track's lines for the log, "[opus] <what>: ...": the end (why, the
  // packets and frames, concealed frames, malformed packets concealed and
  // dropped), the samples kept against the exact length (gate G9 reads it
  // from a whole play's "end" line; a start part of the way in says "from
  // the start on" and the sample asked beside the one landed on, "(exact)"
  // when they agree), the reader's counts (damaged pages,
  // resyncs, gaps), the timeline's corrections, the longest decode call,
  // the longest step (a page slice's read and a decode call at most) and
  // the longest loop() (the generator's part of a pass: gate G6 reads the
  // whole pass from the backend's pass_max; the bench's bursts are longer
  // than a pass), and the decode task's stack high-water since boot (the
  // lowest it has been under any track since the task was made, not this
  // track's: gate G4 benches the fuzz file last, or after a restart), and
  // where the state was.
  void logEnd(const char* what) const;
  // Where the decoder's state is, for the bench's line.
  void describeState(char* buf, size_t size) const;

  uint64_t kept() const { return tl_.kept(); }
  uint32_t maxDecodeUs() const { return maxDecodeUs_; }
  uint32_t maxStepUs() const { return pass_.maxStepUs(); }
  uint32_t maxLoopUs() const { return maxLoopUs_; }

private:
  bool allocBuffers();
  // The decoder's memory: the arena's layout, or a block of its own.
  bool claimState();
  void releaseState();
  static void* allocHigh(size_t bytes, uint32_t* held);
  // One step: the next frame decoded into pcm_ (its kept part at outAt_,
  // outLeft_), or the next piece of a gap's or a malformed packet's fill
  // made there, or the reader's next page (or a slice of it) read; false:
  // the track's end.
  bool decodeNext();
  // One piece of the fill owed (fillLeft_): a concealed frame of fillToc_'s
  // size while fillConceal_ allows, else silence, at most a frame's worth.
  void fillNext();
  // `n` samples made in `out` (decoded, concealed or silence): the
  // timeline's keep, and the kept part expanded (mono) or mapped (family 1)
  // into pcm_ for the output (outAt_, outLeft_).
  void made(int16_t* out, uint32_t n);

  DecoderArena& arena_;
  int layout_;
  Placement placement_ = Placement::Low;
  SourceReader src_;
  AudioFileSource* opened_on_ = nullptr;
  bool opened_ = false;
  bool fromRecord_ = false;  // the last open() was from the cache's record
  bool speaker_ = false;
  char refusal_[96] = "";
  char note_[48] = "";  // refusalNote(): the refusal in a few words
  uint8_t* pageBuf_ = nullptr;    // PSRAM: oggopus::kPageBytes
  uint8_t* packetBuf_ = nullptr;  // PSRAM: oggopus::kMaxPacketBytes
  uint8_t* scratch_ = nullptr;    // PSRAM: oggopus::kFramePacketBytes
  std::optional<oggopus::Reader> reader_;
  oggopus::Timeline tl_;
  oggopus::Frames frames_;
  oggopus::Reader::Packet pkt_;
  // A start part of the way in (the class comment): the plan the next
  // begin() starts by, how long it took to make, and where the start landed.
  oggopus::StartPlan plan_;
  bool planned_ = false;
  uint32_t planUs_ = 0;
  int64_t landedT_ = -1;   // the first kept sample's trimmed index (-1: none yet)
  bool havePacket_ = false;
  uint32_t frameIdx_ = 0;  // the packet's next frame to decode
  uint32_t outAt_ = 0;     // the decoded frame's kept samples still to hand over
  uint32_t outLeft_ = 0;
  bool ended_ = false;
  // The fill before the packet in hand is decoded (the class comment): a
  // gap's, and a malformed packet's own duration.
  int64_t fillLeft_ = 0;
  uint32_t fillConceal_ = 0;
  uint8_t fillToc_ = 0;    // the concealed frames' size: the last decoded frame's TOC, or the malformed packet's
  uint8_t lastToc_ = 0;    // the last decoded frame's TOC
  bool haveToc_ = false;
  bool gapEnded_ = false;  // a gap over kMaxGapSamples ended the track
  // The decoder.
  OpusDecoder* dec_ = nullptr;
  int16_t* pcm_ = nullptr;
  void* own_ = nullptr;      // a block of this track's own (Internal/High, or no arena)
  bool claimed_ = false;     // the arena's block is ours
  uint32_t highHeld_ = 0;    // blocks held to land the High placement
  int channels_ = 2;
  bool mono_ = false;
  bool mapped_ = false;  // a family-1 table other than the identity: L = decoded mapL_, R = decoded mapR_
  uint8_t mapL_ = 0;
  uint8_t mapR_ = 1;
  // This pass's work (the caps and the time).
  uint32_t passSamples_ = 0;
  uint32_t passCalls_ = 0;
  uint32_t passMalformed_ = 0;
  bool passYield_ = false;  // the reader said Pending: the pass ends with nothing in hand
  PassClock pass_{kPassBudgetUs};  // the pass's time rule: the steps timed, the longest kept (this track)
  // Counters since begin().
  uint32_t packets_ = 0;
  uint32_t decodeCalls_ = 0;
  uint32_t concealed_ = 0;   // frames libopus refused, concealed
  uint32_t malformed_ = 0;   // packets with a readable TOC but bad framing, concealed for their duration
  uint32_t dropped_ = 0;     // packets with no TOC to size them by, dropped
  uint32_t skipped_ = 0;     // packets before a plan's preroll, skipped undecoded (M3)
  uint32_t gaps_ = 0;      // packets after a damaged or lost page
  uint32_t gapsFilled_ = 0;  // ... whose gap the reader could size, and so was filled
  uint64_t filled_ = 0;    // ... with this many samples in all (malformed packets' included)
  uint32_t maxDecodeUs_ = 0;
  uint32_t maxLoopUs_ = 0;
  uint32_t openUs_ = 0;
  uint32_t openReads_ = 0;
  uint64_t openBytes_ = 0;
};
