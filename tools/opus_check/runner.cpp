// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// The Ogg Opus reader (lib/core/OggPage, OggOpus, compiled unchanged)
// over real files, with the real decoder: ESP8266Audio's bundled libopus
// (the firmware's, FIXED_POINT, built for the host by
// tools/opus_check/opus_check.py build). It decodes the way the firmware's
// generator does (docs/OPUS.md): every packet split into frames, one
// opus_decode() call per frame, the Timeline's pre-skip and EOS trim, a
// gap after a damaged page filled (concealment for up to two frames, then
// silence) so the length stays exact, a malformed packet with a readable
// TOC concealed for its duration the same way (one with none dropped),
// and checks, bit for bit, that the per-frame output equals a
// whole-packet decode by a second decoder. The
// seek command plans starts part of the way in (the bisection and the
// preroll, as the firmware's M3 will) and compares what they decode with
// the decode from the top. Not part of the firmware or the native tests;
// opus_check.py run compares its output with ffmpeg's.
//
//   runner decode <file.opus> [out.raw] [--corrupt BYTE] [--slice N]
//       key=value lines on stdout; out.raw: the trimmed PCM, s16le, the
//       file's channels, 48 kHz. --corrupt flips one byte of the file
//       first (a damaged page: the resync, the gap fill and the timeline;
//       maxCallBytes is the most one next() call read, a slice and a
//       header once the resync is made in steps: docs/OPUS.md 8.11).
//       --slice N reads the audio pages in slices of N bytes, a Pending
//       between them, as the generator does (its kPageSlice, 8,192: the
//       default here, so the sliced path is what the real files check);
//       0 reads them whole.
//   runner info <file.opus>
//       the open and the tail scan only.
//   runner seek <file.opus> <n> [--seed S] [--preroll MS] [--window N] [--slice N]
//       n starts at random trimmed samples (the seed's), each planned with
//       the preroll (the reader's seek preroll, kSeekPrerollMs, 200 ms;
//       600: a resume) and decoded for the
//       window (4,096 samples) after the target, against the decode from
//       the top: whether each landed on its sample, how many were bit-exact
//       over the window, the worst |difference| and SNR, the probes and
//       reads a plan cost.
//   runner plan <file.opus> <ms> [--sample S] [--preroll MS] [--window N] [--slice N] [out.raw]
//       one start, planned as the firmware plans it (docs/OPUS.md section
//       9): at `ms` with the tail rule (Reader::planStartMs(), the seek
//       preroll by default), or, with --sample, at a trimmed sample as
//       a resume anchor's (planStart(), the 600 ms resume preroll by
//       default; the anchor made and checked as the backend does). It
//       prints the plan (the page reading starts at and its granule, where
//       decoding and keeping begin, the probes and reads: the figures the
//       device's `[audio] Opus: starting ... in` line names for the same
//       file and second, the plan being a function of the file alone),
//       then decodes the window (4,096 samples; 0: to the end) from the
//       target against the decode from the top (landed, bit-exact, the
//       max |difference| and SNR), and writes the kept samples to out.raw
//       (s16le, the file's channels, 48 kHz) for opus_check.py plan to
//       compare with ffmpeg's decode from the top at the same offset.
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "OggOpus.h"
#include "OggPage.h"
#include "TrackSeek.h"

extern "C" {
#include "opus.h"
}

namespace {

// The file in memory, read as the firmware reads the card: counted.
class HostFile : public trackseek::FileReader {
public:
  bool load(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    data.resize(static_cast<size_t>(n));
    const size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    return got == data.size();
  }
  uint32_t size() const { return static_cast<uint32_t>(data.size()); }
  uint32_t readAt(uint32_t offset, uint8_t* buf, uint32_t n) override {
    ++reads;
    if (offset >= data.size()) return 0;
    if (n > data.size() - offset) n = static_cast<uint32_t>(data.size() - offset);
    std::memcpy(buf, data.data() + offset, n);
    bytes += n;
    return n;
  }
  std::vector<uint8_t> data;
  uint32_t reads = 0;
  uint64_t bytes = 0;
};

OpusDecoder* makeDecoder(const oggopus::Head& h) {
  OpusDecoder* d = static_cast<OpusDecoder*>(std::malloc(static_cast<size_t>(opus_decoder_get_size(h.channels))));
  opus_decoder_init(d, 48000, h.channels);
  opus_decoder_ctl(d, OPUS_SET_GAIN(h.gain));
  return d;
}

// One decode of the track as the generator does it, from the reader's
// current position with the timeline given (the top, or a plan).
struct Decode {
  std::vector<int16_t> out;  // the kept PCM
  uint64_t packets = 0, frames = 0, decoded = 0, concealed = 0, malformed = 0, dropped = 0, skipped = 0, filled = 0,
           gaps = 0;
  uint64_t mismatches = 0, mismatchedPackets = 0;  // the per-frame split against whole packets
  uint32_t maxFrameSamples = 0, maxPacketBytes = 0;
  uint32_t maxCallBytes = 0;  // the most one next() call read (a slice and a header; a resync's chunk)
  uint64_t silkPackets = 0, hybridPackets = 0, celtPackets = 0;  // by the TOC's configuration
  bool gapEnded = false;
  int64_t firstKeptT = -1;  // the first kept sample's trimmed index
};

// `whole`: a second decoder fed whole packets, for the split check (null:
// skip it). `stopAfterKept`: stop once this many samples are kept (0: all).
Decode decodeTrack(oggopus::Reader& r, oggopus::Timeline& tl, OpusDecoder* dec, OpusDecoder* whole, int ch,
                   uint64_t stopAfterKept) {
  Decode d;
  std::vector<int16_t> pcm(oggopus::kMaxFrameSamples * 2);
  std::vector<int16_t> pcmWhole(oggopus::kMaxPacketSamples * 2);
  std::vector<int16_t> packetPcm;
  std::vector<uint8_t> scratch(oggopus::kFramePacketBytes);
  oggopus::Reader::Packet p;
  oggopus::Frames fr;
  uint8_t lastToc = 0;
  bool haveToc = false;
  auto keep = [&](int got) {
    const int64_t pos = tl.position();
    const oggopus::Timeline::Keep k = tl.decoded(static_cast<uint32_t>(got));
    if (k.take) {
      if (d.firstKeptT < 0) d.firstKeptT = pos + k.skip;
      d.out.insert(d.out.end(), pcm.begin() + static_cast<long>(k.skip) * ch,
                   pcm.begin() + static_cast<long>(k.skip + k.take) * ch);
    }
  };
  for (;;) {
    const uint64_t bytes0 = r.pages().bytesRead();
    const oggopus::Reader::Next n = r.next(&p);
    const auto callBytes = static_cast<uint32_t>(r.pages().bytesRead() - bytes0);
    if (callBytes > d.maxCallBytes) d.maxCallBytes = callBytes;
    if (n == oggopus::Reader::Next::End) break;
    if (n == oggopus::Reader::Next::Pending) continue;  // (the generator ends its pass here)
    ++d.packets;
    tl.packet(p);
    if (p.bytes > d.maxPacketBytes) d.maxPacketBytes = p.bytes;
    const bool framed = p.samples > 0 && oggopus::splitPacket(p.data, p.bytes, &fr);
    if (p.samples < 0) {
      // No TOC to size it by: dropped, the page's granule puts the count
      // right (the generator does the same).
      ++d.dropped;
      tl.packetDone();
      continue;
    }
    // The gap fill: concealment for up to two frames of the last TOC's
    // size, silence after, as the generator does it. A malformed packet
    // with a readable TOC (its framing wrong: libopus would refuse it) is
    // concealed for its TOC's duration in the same fill, by its own TOC's
    // frame size; the whole-packet decoder gets the same concealment
    // calls so the two stay in step (their output isn't compared there).
    const int64_t gap = tl.gap();
    if (gap > oggopus::kMaxGapSamples) {
      d.gapEnded = true;
      break;
    }
    if (gap > 0) ++d.gaps;
    int64_t fill = gap;
    uint32_t conceal = gap > 0 && haveToc ? 2 : 0;
    uint8_t fillToc = lastToc;
    if (!framed) {
      ++d.malformed;
      fill += p.samples;
      if (conceal == 0) {
        conceal = 2;
        fillToc = p.data[0];
      }
    }
    for (int64_t left = fill; left > 0;) {
      uint32_t n = left > oggopus::kMaxFrameSamples ? oggopus::kMaxFrameSamples : static_cast<uint32_t>(left);
      if (conceal > 0 && oggopus::frameSamples(fillToc) <= static_cast<uint32_t>(left)) {
        n = oggopus::frameSamples(fillToc);
        const int got = opus_decode(dec, nullptr, 0, pcm.data(), static_cast<int>(n), 0);
        if (got != static_cast<int>(n)) std::memset(pcm.data(), 0, n * ch * sizeof(int16_t));
        if (whole) opus_decode(whole, nullptr, 0, pcmWhole.data(), static_cast<int>(n), 0);
        --conceal;
      } else {
        conceal = 0;
        std::memset(pcm.data(), 0, n * ch * sizeof(int16_t));
      }
      keep(static_cast<int>(n));
      d.filled += n;
      left -= n;
    }
    if (!framed) {
      tl.packetDone();
      continue;
    }
    if (!tl.wanted(static_cast<uint32_t>(p.samples))) {
      tl.skipped(static_cast<uint32_t>(p.samples));
      ++d.skipped;
      tl.packetDone();
      continue;
    }
    const uint32_t config = fr.toc >> 3;
    if (config < 12) {
      ++d.silkPackets;
    } else if (config < 16) {
      ++d.hybridPackets;
    } else {
      ++d.celtPackets;
    }
    packetPcm.clear();
    for (uint32_t i = 0; i < fr.count; ++i) {
      const uint32_t n = oggopus::framePacket(fr, i, scratch.data());
      int got = opus_decode(dec, scratch.data(), static_cast<opus_int32>(n), pcm.data(),
                            static_cast<int>(oggopus::kMaxFrameSamples), 0);
      if (got < 0) {
        ++d.concealed;
        got = opus_decode(dec, nullptr, 0, pcm.data(), static_cast<int>(oggopus::frameSamples(fr.toc)), 0);
        if (got < 0) got = 0;
      }
      ++d.frames;
      d.decoded += static_cast<uint64_t>(got);
      if (static_cast<uint32_t>(got) > d.maxFrameSamples) d.maxFrameSamples = static_cast<uint32_t>(got);
      if (whole) packetPcm.insert(packetPcm.end(), pcm.begin(), pcm.begin() + got * ch);
      keep(got);
    }
    lastToc = fr.toc;
    haveToc = true;
    tl.packetDone();
    if (whole) {
      // The whole packet, by the other decoder.
      int gotWhole = opus_decode(whole, p.data, static_cast<opus_int32>(p.bytes), pcmWhole.data(),
                                 static_cast<int>(oggopus::kMaxPacketSamples), 0);
      if (gotWhole < 0) {
        gotWhole = opus_decode(whole, nullptr, 0, pcmWhole.data(), p.samples, 0);
        if (gotWhole < 0) gotWhole = 0;
      }
      if (static_cast<size_t>(gotWhole) * ch != packetPcm.size()) {
        ++d.mismatchedPackets;
        d.mismatches += packetPcm.size() / ch;
      } else {
        size_t bad = 0;
        for (size_t i = 0; i < packetPcm.size(); ++i) bad += packetPcm[i] != pcmWhole[i] ? 1 : 0;
        if (bad) ++d.mismatchedPackets;
        d.mismatches += bad;
      }
    }
    if (tl.finished()) break;
    if (stopAfterKept && d.out.size() / ch >= stopAfterKept) break;
  }
  return d;
}

[[noreturn]] void usage() {
  std::fprintf(stderr,
               "usage: runner decode <file.opus> [out.raw] [--corrupt BYTE] [--slice N]\n"
               "       runner info <file.opus>\n"
               "       runner seek <file.opus> <n> [--seed S] [--preroll MS] [--window N] [--slice N]\n"
               "       runner plan <file.opus> <ms> [--sample S] [--preroll MS] [--window N] [--slice N] [out.raw]\n");
  std::exit(2);
}

// The window after a start compared with the decode from the top (the
// seek and plan commands): the max |difference| and the SNR over `n`
// samples (frames) of `ours` against `ref` from frame `at`.
struct Window {
  uint32_t maxDiff = 0;
  double snr = 999.0;
};
Window compareWindow(const std::vector<int16_t>& ours, const std::vector<int16_t>& ref, uint64_t at, size_t n, int ch) {
  Window w;
  double err = 0.0, sig = 0.0;
  for (size_t k = 0; k < n * ch; ++k) {
    const int diff = std::abs(static_cast<int>(ours[k]) - static_cast<int>(ref[at * ch + k]));
    if (static_cast<uint32_t>(diff) > w.maxDiff) w.maxDiff = static_cast<uint32_t>(diff);
    err += static_cast<double>(diff) * diff;
    sig += static_cast<double>(ref[at * ch + k]) * ref[at * ch + k];
  }
  w.snr = err == 0.0 ? 999.0 : (sig > 0.0 ? 10.0 * std::log10(sig / err) : 0.0);
  return w;
}

// The generator's page slice (OpusGenerator::kPageSlice; src/ is Arduino's
// and isn't included here): the default, so the real files go through the
// sliced reads the firmware makes.
constexpr uint32_t kGeneratorSlice = 8192;

// An anchor check's verdict as one token (opus_check.py reads key=value
// pairs split on spaces: "in its last 5 s" wouldn't survive).
const char* anchorToken(oggopus::AnchorCheck c) {
  switch (c) {
    case oggopus::AnchorCheck::Ok: return "ok";
    case oggopus::AnchorCheck::Kind: return "kind";
    case oggopus::AnchorCheck::Size: return "size";
    case oggopus::AnchorCheck::Length: return "length";
    case oggopus::AnchorCheck::Tail: return "tail";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) usage();
  const std::string cmd = argv[1];
  const char* path = argv[2];
  const char* outPath = nullptr;
  int64_t corrupt = -1;
  uint32_t seeks = 0, seed = 1, prerollMs = oggopus::kSeekPrerollMs, window = 4096, slice = kGeneratorSlice;
  uint32_t planMs = 0;
  int64_t planSample = -1;  // plan: --sample S (a resume anchor's sample) instead of the ms
  bool prerollGiven = false;
  int first = 3;
  if (cmd == "seek" || cmd == "plan") {
    if (argc < 4) usage();
    if (cmd == "seek") {
      seeks = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0));
    } else {
      planMs = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0));
    }
    first = 4;
  }
  for (int i = first; i < argc; ++i) {
    if (std::strcmp(argv[i], "--corrupt") == 0 && i + 1 < argc) {
      corrupt = std::strtoll(argv[++i], nullptr, 0);
    } else if (std::strcmp(argv[i], "--slice") == 0 && i + 1 < argc) {
      slice = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
    } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
    } else if (std::strcmp(argv[i], "--preroll") == 0 && i + 1 < argc) {
      prerollMs = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
      prerollGiven = true;
    } else if (std::strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
      window = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
    } else if (std::strcmp(argv[i], "--sample") == 0 && i + 1 < argc && cmd == "plan") {
      planSample = std::strtoll(argv[++i], nullptr, 0);
    } else if (!outPath && (cmd == "decode" || cmd == "plan")) {
      outPath = argv[i];
    } else {
      usage();
    }
  }
  // A plan by an anchor's sample is a resume: its preroll unless given.
  if (cmd == "plan" && planSample >= 0 && !prerollGiven) prerollMs = oggopus::kResumePrerollMs;
  HostFile file;
  if (!file.load(path)) {
    std::fprintf(stderr, "runner: can't read %s\n", path);
    return 1;
  }
  if (corrupt >= 0 && static_cast<uint64_t>(corrupt) < file.data.size()) file.data[static_cast<size_t>(corrupt)] ^= 0x5A;

  std::vector<uint8_t> pageBuf(oggopus::kPageBytes);
  std::vector<uint8_t> packetBuf(oggopus::kMaxPacketBytes);
  oggopus::Reader r(file, file.size(), pageBuf.data(), packetBuf.data());
  r.setReadSlice(slice);  // (the headers, the tail scan and the plans read whole pages whatever it says)
  const oggopus::Reader::Open o = r.open();
  char text[128];
  static const char* const kOpen[] = {"ok",      "notOgg",   "notOpus", "multiplexed", "badHead", "unsupported",
                                      "noAudio", "badStart", "shortFrames"};
  std::printf("open=%s\n", kOpen[static_cast<int>(o)]);
  std::printf("refusal=%s\n", r.refusal(text, sizeof(text)));
  std::printf("codec=%s\n", oggopus::codecName(r.codec()));
  std::printf("openReads=%u openBytes=%" PRIu64 "\n", file.reads, file.bytes);
  if (o != oggopus::Reader::Open::Ok) return 0;
  const oggopus::Head& h = r.head();
  std::printf("version=%u channels=%u preSkip=%u inputRate=%u gain=%d family=%u streams=%u coupled=%u map=%u,%u mapped=%d\n",
              h.version, h.channels, h.preSkip, h.inputRate, h.gain, h.family, h.streams, h.coupled, h.map[0], h.map[1],
              h.mapped() ? 1 : 0);
  std::printf("serial=%08x firstAudioPage=%u g0=%" PRId64 " tagsBytes=%u tagsPages=%u\n", r.serial(), r.firstAudioPage(),
              r.g0(), r.tagsBytes(), r.tagsPages());
  file.reads = 0;
  file.bytes = 0;
  const bool tail = r.scanTail();
  std::printf("tail=%d lastGranule=%" PRId64 " lengthSamples=%" PRIu64 " lengthMs=%u chained=%d linkEnd=%u fileSize=%u "
              "tailReads=%u tailBytes=%" PRIu64 "\n",
              tail ? 1 : 0, r.lastGranule(), r.lengthSamples(), r.lengthMs(), r.chained() ? 1 : 0, r.linkEnd(),
              file.size(), file.reads, file.bytes);
  std::printf("decoderState=%d\n", opus_decoder_get_size(h.channels));
  if (cmd == "info") return 0;
  const int ch = h.channels;

  if (cmd == "decode") {
    // Two decoders: one fed frame by frame (the firmware's way), one whole
    // packets; their outputs must agree bit for bit.
    OpusDecoder* byFrame = makeDecoder(h);
    OpusDecoder* whole = makeDecoder(h);
    oggopus::Timeline tl;
    tl.start(r.g0(), h.preSkip);
    file.reads = 0;
    file.bytes = 0;
    const Decode d = decodeTrack(r, tl, byFrame, whole, ch, 0);
    static const char* const kEnd[] = {"none", "eos", "truncated", "chained"};
    std::printf("end=%s finished=%d gapEnded=%d packets=%" PRIu64 " frames=%" PRIu64 " malformed=%" PRIu64
                " dropped=%" PRIu64 " concealed=%" PRIu64 " decodedSamples=%" PRIu64 " keptSamples=%" PRIu64
                " filled=%" PRIu64 " gaps=%" PRIu64 " maxFrameSamples=%u maxPacketBytes=%u\n",
                kEnd[static_cast<int>(r.ended())], tl.finished() ? 1 : 0, d.gapEnded ? 1 : 0, d.packets, d.frames,
                d.malformed, d.dropped, d.concealed, d.decoded, tl.kept(), d.filled, d.gaps, d.maxFrameSamples,
                d.maxPacketBytes);
    std::printf("corrections=%u slip=%" PRId64 " position=%" PRId64 "\n", tl.corrections(), tl.slip(), tl.position());
    const oggopus::Reader::Stats& s = r.stats();
    std::printf("pages=%u badPages=%u resyncs=%u resyncBytes=%u sequenceGaps=%u droppedPartials=%u foreignPages=%u "
                "oversized=%u gapsSized=%u\n",
                s.pages, s.badPages, s.resyncs, s.resyncBytes, s.sequenceGaps, s.droppedPartials, s.foreignPages,
                s.oversized, s.gapsSized);
    std::printf("playReads=%u playBytes=%" PRIu64 " maxCallBytes=%u\n", file.reads, file.bytes, d.maxCallBytes);
    std::printf("splitExact=%d splitMismatchedSamples=%" PRIu64 " splitMismatchedPackets=%" PRIu64 "\n",
                d.mismatches == 0 ? 1 : 0, d.mismatches, d.mismatchedPackets);
    std::printf("outSamples=%zu\n", d.out.size() / static_cast<size_t>(ch));
    if (outPath) {
      FILE* f = std::fopen(outPath, "wb");
      if (!f) {
        std::fprintf(stderr, "runner: can't write %s\n", outPath);
        return 1;
      }
      std::fwrite(d.out.data(), sizeof(int16_t), d.out.size(), f);
      std::fclose(f);
    }
    std::free(byFrame);
    std::free(whole);
    return 0;
  }
  if (cmd != "seek" && cmd != "plan") usage();

  if (cmd == "plan") {
    // One start as the firmware makes it: the plan, then the decode by it
    // against the decode from the top (the reference: the target and the
    // window, or the whole track when the window is 0).
    OpusDecoder* dec = makeDecoder(h);
    oggopus::Timeline tl;
    oggopus::StartPlan plan;
    uint32_t landMs = 0;
    bool byAnchor = false;
    if (planSample >= 0) {
      // A resume anchor's sample: the anchor made and checked as the
      // backend does (oggopus::makeAnchor(), checkAnchor(): the size, the
      // length, the tail rule), then planned by its sample; one the check
      // refuses falls back to the ms, as the backend's "by its second".
      const ResumeAnchor anchor = oggopus::makeAnchor(static_cast<uint64_t>(planSample), file.size(), r.lengthSamples());
      const oggopus::AnchorCheck c = oggopus::checkAnchor(anchor, file.size(), r.lengthSamples());
      std::printf("anchorCheck=%s\n", anchorToken(c));
      byAnchor = c == oggopus::AnchorCheck::Ok;
      if (!byAnchor && !prerollGiven) prerollMs = oggopus::kSeekPrerollMs;
    }
    file.reads = 0;
    file.bytes = 0;
    if (byAnchor) {
      r.planStart(static_cast<uint64_t>(planSample), prerollMs * 48, &plan);
      landMs = static_cast<uint32_t>(plan.target / 48);
    } else {
      landMs = r.planStartMs(planMs, prerollMs, &plan);
    }
    std::printf("byAnchor=%d\n", byAnchor ? 1 : 0);
    std::printf("plan: fromTop=%d pageOffset=%u skipPage=%d sequence=%u k=%" PRId64 " decodeFrom=%" PRId64
                " keepFrom=%" PRId64 " target=%" PRIu64 " landMs=%u prerollMs=%u probes=%u reads=%u bytes=%" PRIu64
                " planReads=%u planBytes=%" PRIu64 "\n",
                plan.fromTop ? 1 : 0, plan.pageOffset, plan.skipPage ? 1 : 0, plan.sequence, plan.k, plan.decodeFrom,
                plan.keepFrom, plan.target, landMs, prerollMs, plan.probes, plan.reads, plan.bytes, file.reads,
                file.bytes);
    // The reference from the top, as far as the window needs.
    const uint64_t target = plan.target;
    tl.start(r.g0(), h.preSkip);
    r.restart();
    const Decode ref = decodeTrack(r, tl, dec, nullptr, ch, window ? target + window : 0);
    const uint64_t refLength = ref.out.size() / static_cast<size_t>(ch);
    // A damaged file: the reference filled its gaps as the firmware does
    // (the timeline exact), where ffmpeg drops the lost samples and runs
    // short from there (opus_check.py plan compares against the in-process
    // decode alone then).
    std::printf("refFilled=%" PRIu64 " refGaps=%" PRIu64 " refBadPages=%u\n", ref.filled, ref.gaps, r.stats().badPages);
    // The start by the plan: the reader at its page, the decoder reset (the
    // firmware initialises it afresh at every begin()), the timeline by it.
    r.startAt(plan);
    opus_decoder_ctl(dec, OPUS_RESET_STATE);
    tl.start(plan);
    file.reads = 0;
    file.bytes = 0;
    const Decode d = decodeTrack(r, tl, dec, nullptr, ch, window);
    const uint64_t got = d.out.size() / static_cast<size_t>(ch);
    const uint64_t decodedBefore = d.decoded + d.filled - got;
    std::printf("landed=%" PRId64 " target=%" PRIu64 " outSamples=%" PRIu64 " keptSamples=%" PRIu64
                " decodedBefore=%" PRIu64 " skipped=%" PRIu64 " filled=%" PRIu64 " playReads=%u playBytes=%" PRIu64
                " maxCallBytes=%u finished=%d refSamples=%" PRIu64 "\n",
                d.firstKeptT, target, got, tl.kept(), decodedBefore, d.skipped, d.filled, file.reads, file.bytes,
                d.maxCallBytes, tl.finished() ? 1 : 0, refLength);
    const bool landed = d.firstKeptT == static_cast<int64_t>(target);
    size_t n = window ? window : static_cast<size_t>(got);
    if (n > got) n = static_cast<size_t>(got);
    if (target + n > refLength) n = target < refLength ? static_cast<size_t>(refLength - target) : 0;
    Window w;
    if (landed && n > 0) w = compareWindow(d.out, ref.out, target, n, ch);
    std::printf("window=%u compared=%zu bitExact=%d maxDiff=%u snr=%.1f\n", window, n,
                landed && n > 0 && w.maxDiff == 0 ? 1 : 0, w.maxDiff, landed && n > 0 ? w.snr : 0.0);
    if (outPath) {
      FILE* f = std::fopen(outPath, "wb");
      if (!f) {
        std::fprintf(stderr, "runner: can't write %s\n", outPath);
        return 1;
      }
      std::fwrite(d.out.data(), sizeof(int16_t), d.out.size(), f);
      std::fclose(f);
    }
    std::free(dec);
    return 0;
  }

  // The reference: the whole track from the top.
  OpusDecoder* dec = makeDecoder(h);
  oggopus::Timeline tl;
  tl.start(r.g0(), h.preSkip);
  const Decode ref = decodeTrack(r, tl, dec, nullptr, ch, 0);
  const uint64_t length = ref.out.size() / static_cast<size_t>(ch);
  std::printf("refSamples=%" PRIu64 " preroll=%u window=%u modes=%s silk=%" PRIu64 " hybrid=%" PRIu64 " celt=%" PRIu64
              "\n",
              length, prerollMs, window,
              ref.silkPackets == 0 && ref.hybridPackets == 0 ? "celt" : ref.celtPackets == 0 ? "silk" : "mixed",
              ref.silkPackets, ref.hybridPackets, ref.celtPackets);
  if (length <= window + 1) {
    std::printf("seeks=0 note=too short\n");
    return 0;
  }
  // The anchor model: a resume point 1 s in (a file under 7 s has none
  // outside the tail rule), checked against the file as opened.
  if (r.lengthMs() > 7000) {
    const ResumeAnchor anchor = oggopus::makeAnchor(48000, file.size(), r.lengthSamples());
    std::printf("anchorCheck=%s\n", anchorToken(oggopus::checkAnchor(anchor, file.size(), r.lengthSamples())));
  }
  uint32_t rng = seed * 2654435761u + 7u;
  uint32_t landed = 0, exact = 0, fromTop = 0, maxDiffWorst = 0, probesMax = 0, readsMax = 0;
  uint64_t probesSum = 0, readsSum = 0, bytesSum = 0, bytesMax = 0, decodedBeforeMax = 0, decodedBeforeSum = 0;
  double snrWorst = 999.0, snrSum = 0.0;
  const uint32_t prerollSamples = prerollMs * 48;
  for (uint32_t i = 0; i < seeks; ++i) {
    rng = rng * 1103515245u + 12345u;
    const uint64_t t = (static_cast<uint64_t>(rng >> 8) * (length - window - 1)) / (1u << 24);
    oggopus::StartPlan plan;
    file.reads = 0;
    file.bytes = 0;
    r.planStart(t, prerollSamples, &plan);
    r.startAt(plan);
    opus_decoder_ctl(dec, OPUS_RESET_STATE);
    tl.start(plan);
    const Decode d = decodeTrack(r, tl, dec, nullptr, ch, window);
    if (plan.fromTop) ++fromTop;
    probesSum += plan.probes;
    readsSum += file.reads;
    bytesSum += file.bytes;
    if (plan.probes > probesMax) probesMax = plan.probes;
    if (file.reads > readsMax) readsMax = file.reads;
    if (file.bytes > bytesMax) bytesMax = file.bytes;
    if (d.firstKeptT != static_cast<int64_t>(t)) {
      std::printf("seek %u: target %" PRIu64 " landed %" PRId64 "\n", i, t, d.firstKeptT);
      continue;
    }
    ++landed;
    // The window against the reference.
    const size_t n = static_cast<size_t>(window) * ch;
    if (d.out.size() < n) {
      std::printf("seek %u: target %" PRIu64 " gave %zu samples\n", i, t, d.out.size() / ch);
      continue;
    }
    const Window w = compareWindow(d.out, ref.out, t, window, ch);
    const uint32_t maxd = w.maxDiff;
    const double snr = w.snr;
    if (maxd == 0) ++exact;
    if (maxd > maxDiffWorst) maxDiffWorst = maxd;
    if (snr < snrWorst) snrWorst = snr;
    snrSum += snr > 200.0 ? 200.0 : snr;
    // The samples decoded before the target (the preroll and the part of
    // the first packet before it): everything decoded or filled, less
    // what was kept.
    const uint64_t decodedBefore = d.decoded + d.filled - d.out.size() / static_cast<size_t>(ch);
    decodedBeforeSum += decodedBefore;
    if (decodedBefore > decodedBeforeMax) decodedBeforeMax = decodedBefore;
  }
  std::printf("seeks=%u landed=%u bitExact=%u fromTop=%u maxDiffWorst=%u snrWorst=%.1f snrMean=%.1f probesMean=%.2f "
              "probesMax=%u readsMean=%.1f readsMax=%u bytesMean=%" PRIu64 " bytesMax=%" PRIu64
              " decodedBeforeMean=%" PRIu64 " decodedBeforeMax=%" PRIu64 "\n",
              seeks, landed, exact, fromTop, maxDiffWorst, landed ? snrWorst : 0.0, landed ? snrSum / landed : 0.0,
              seeks ? static_cast<double>(probesSum) / seeks : 0.0, probesMax,
              seeks ? static_cast<double>(readsSum) / seeks : 0.0, readsMax, seeks ? bytesSum / seeks : 0, bytesMax,
              landed ? decodedBeforeSum / landed : 0, decodedBeforeMax);
  std::free(dec);
  return 0;
}
