// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "OggOpus.h"

#include <cstdio>
#include <cstring>

#include "TrackSeek.h"  // trackseek::startMs(): the tail rule

namespace oggopus {

namespace {

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// A frame size in a code-2 or VBR code-3 packet (opus.c's parse_size):
// one byte under 252, else two. Returns the bytes it took; -1: cut off.
int32_t parseSize(const uint8_t* d, int32_t len, int32_t* size) {
  if (len < 1) return -1;
  if (d[0] < 252) {
    *size = d[0];
    return 1;
  }
  if (len < 2) return -1;
  *size = 4 * d[1] + d[0];
  return 2;
}

// The OpusTags skip gives up after this many pages (a 2 MB picture is
// ~32 pages of 64 KB; ffmpeg writes ~1 KB pages for small tags): 4,096
// pages hold a 256 MB comment header, and cost at most 4,096 header reads
// of 290 bytes, 1.2 MB, on a crafted file of empty tags pages.
constexpr uint32_t kMaxTagPages = 4096;
// One next() call's share: after this many pages, or this many bytes of
// reads, that yielded no packet (empty pages, another stream's, the pages
// of one endless packet) it returns Pending and the next call carries on,
// so the decode task's pass ends (the class comment). 64 empty pages are
// 64 small reads; 128 KB is two 64 KB pages of another stream.
constexpr uint32_t kNextPages = 64;
constexpr uint64_t kNextBudget = 128u * 1024;
// How far past the headers the first audio page is looked for when the
// page there is damaged, and how many bytes that search may read.
constexpr uint32_t kFirstAudioScan = 2 * 65536;
constexpr uint32_t kFirstAudioBudget = 8 * 65536;
// The same while playing, after a damaged or lost page: the next good page
// of ours within 1 MB (16 s at 510k, 65 s at 128k: a gap over that ends
// the track, as the research's 10 s rule means), reading at most 2 MB for
// it. The budget is what bounds a crafted file (headers with the right
// version and serial, a wrong CRC and 65 KB claimed each: a page's read
// per 282 bytes): ~2 s of SD time at worst, not minutes.
constexpr uint32_t kResyncScan = 1u << 20;
constexpr uint32_t kResyncBudget = 2u << 20;
// The tail scan's windows (the class comment; docs/OPUS.md section 10):
// the file's last 16 KB as one chunk (an mStream page is ~16 KB at 128k
// and the last page is what the encoder had left, so the last page's
// header is in it nearly always: one read, the page checked in memory),
// then its last 64 KB as one chunk (a 64 KB page's: one read, in memory
// too), then its last 73 KB walked in 16 KB chunks with the page taken
// read whole (a junk tail of up to 8 KB after a page of the largest
// size). How many bytes a window's walk may read: four times the window
// (a legitimate walk reads a chunk per chunk of pages, whatever their
// size; a tail of crafted 27-byte headers each followed by a junk byte
// makes each step a fresh chunk).
constexpr uint32_t kTailChunk = 16384;
constexpr uint32_t kTailShort = kTailChunk;
constexpr uint32_t kTailMid = ogg::kMaxPageBytes;
constexpr uint32_t kTailLong = ogg::kMaxPageBytes + 8192;
constexpr uint32_t kTailBudgetMul = 4;
// The bisections' probes: a seek's scan for the next header in 4 KB
// chunks (a page at 128k is ~16 KB, so a probe into one costs a chunk or
// two, and nothing more: the guess itself is never a page's start, so no
// header is read there; the research's prototype measured ~30-40 KB a
// seek this way), the tail's in the tail's chunks; what one probe may read
// (a crafted body of headers claiming 65 KB pages: two of them and a
// chunk), how many steps a probe walks by headers, how many probes a plan
// or a tail bisection makes, and what a whole plan may read before it
// settles for the page it has (an earlier start, still exact).
constexpr uint32_t kSeekChunk = 4096;
constexpr uint64_t kProbeBudget = 2u * ogg::kMaxPageBytes + kTailChunk;
constexpr uint32_t kProbeWalk = 64;
constexpr uint32_t kMaxProbes = 48;
constexpr uint64_t kPlanBudget = 1u << 20;
// The bisection's probe aimed a little early: a page found just past the
// target costs another probe, one just before it a short walk.
constexpr uint32_t kAimEarly = 4096;

// A granule position that can be believed (kMaxGranule).
bool plausible(int64_t granule) { return granule >= 0 && granule <= kMaxGranule; }

}  // namespace

Codec codecOf(const uint8_t* p, size_t n) {
  if (n >= 8 && std::memcmp(p, "OpusHead", 8) == 0) return Codec::Opus;
  if (n >= 7 && p[0] == 0x01 && std::memcmp(p + 1, "vorbis", 6) == 0) return Codec::Vorbis;
  if (n >= 5 && p[0] == 0x7F && std::memcmp(p + 1, "FLAC", 4) == 0) return Codec::Flac;
  if (n >= 8 && std::memcmp(p, "Speex   ", 8) == 0) return Codec::Speex;
  if (n >= 7 && p[0] == 0x80 && std::memcmp(p + 1, "theora", 6) == 0) return Codec::Theora;
  return Codec::Other;
}

const char* codecName(Codec c) {
  switch (c) {
    case Codec::None: return "none";
    case Codec::Opus: return "Opus";
    case Codec::Vorbis: return "Vorbis";
    case Codec::Flac: return "FLAC";
    case Codec::Speex: return "Speex";
    case Codec::Theora: return "Theora";
    case Codec::Other: return "unknown";
  }
  return "?";
}

HeadCheck parseHead(const uint8_t* p, size_t n, Head* out) {
  *out = Head{};
  if (n < 19 || std::memcmp(p, "OpusHead", 8) != 0) return HeadCheck::Short;
  Head& h = *out;
  h.version = p[8];
  h.channels = p[9];
  h.preSkip = le16(p + 10);
  h.inputRate = le32(p + 12);
  h.gain = static_cast<int16_t>(le16(p + 16));
  h.family = p[18];
  if (h.version >> 4) return HeadCheck::Version;
  if (h.channels == 0) return HeadCheck::Channels;
  if (h.family == 0) {
    h.streams = 1;
    h.coupled = h.channels == 2 ? 1 : 0;
    return h.channels > 2 ? HeadCheck::Streams : HeadCheck::Ok;
  }
  if (h.family != 1) return HeadCheck::Family;
  if (n < 21u + h.channels) return HeadCheck::Short;
  h.streams = p[19];
  h.coupled = p[20];
  for (uint32_t i = 0; i < 2 && i < h.channels; ++i) h.map[i] = p[21 + i];
  if (h.channels > 2 || h.streams != 1 || h.coupled != h.channels - 1) return HeadCheck::Streams;
  // The one stream's channels: 2 when coupled, else 1 (255 would be silence).
  const uint8_t have = static_cast<uint8_t>(h.coupled + 1);
  for (uint32_t i = 0; i < h.channels; ++i) {
    if (h.map[i] >= have) return HeadCheck::Mapping;
  }
  return HeadCheck::Ok;
}

const char* headCheckName(HeadCheck c) {
  switch (c) {
    case HeadCheck::Ok: return "ok";
    case HeadCheck::Short: return "too short";
    case HeadCheck::Version: return "the version";
    case HeadCheck::Channels: return "0 channels";
    case HeadCheck::Family: return "the mapping family";
    case HeadCheck::Streams: return "the streams";
    case HeadCheck::Mapping: return "the mapping table";
  }
  return "?";
}

uint32_t frameSamples(uint8_t toc) {
  const uint32_t config = toc >> 3;
  if (config >= 16) return 120u << (config & 3);              // CELT: 2.5, 5, 10, 20 ms
  if (config >= 12) return (config & 1) ? 960 : 480;          // hybrid: 10 or 20 ms
  const uint32_t n = config & 3;                               // SILK: 10, 20, 40, 60 ms
  return n == 3 ? 2880 : 480u << n;
}

int32_t frameCount(const uint8_t* p, size_t n) {
  if (n < 1) return -1;
  const uint8_t code = p[0] & 3;
  if (code == 0) return 1;
  if (code != 3) return 2;
  if (n < 2) return -1;
  const int32_t count = p[1] & 0x3F;
  if (count == 0) return -1;
  if (static_cast<uint32_t>(count) * frameSamples(p[0]) > kMaxPacketSamples) return -1;
  return count;
}

int32_t packetSamples(const uint8_t* p, size_t n) {
  const int32_t count = frameCount(p, n);
  return count < 0 ? -1 : count * static_cast<int32_t>(frameSamples(p[0]));
}

bool splitPacket(const uint8_t* p, uint32_t n, Frames* out) {
  if (n == 0) return false;
  const uint8_t toc = p[0];
  const uint32_t fs = frameSamples(toc);
  const uint8_t* d = p + 1;
  int32_t len = static_cast<int32_t>(n) - 1;
  int32_t last = len;
  uint32_t count = 1;
  switch (toc & 3) {
    case 0:
      count = 1;
      break;
    case 1:  // two frames of the same size
      count = 2;
      if (len & 1) return false;
      last = len / 2;
      out->size[0] = static_cast<uint16_t>(last);
      break;
    case 2: {  // two frames, the first's size in front
      count = 2;
      int32_t s0 = 0;
      const int32_t bytes = parseSize(d, len, &s0);
      if (bytes < 0) return false;
      len -= bytes;
      if (s0 > len) return false;
      d += bytes;
      out->size[0] = static_cast<uint16_t>(s0);
      last = len - s0;
      break;
    }
    default: {  // code 3: the count byte, padding, then the sizes (VBR) or equal frames
      if (len < 1) return false;
      const uint8_t ch = *d++;
      count = ch & 0x3F;
      if (count == 0 || fs * count > kMaxPacketSamples) return false;
      --len;
      if (ch & 0x40) {
        uint32_t pad = 0;
        int32_t b;
        do {
          if (len <= 0) return false;
          b = *d++;
          --len;
          const int32_t tmp = b == 255 ? 254 : b;
          len -= tmp;
          pad += static_cast<uint32_t>(tmp);
        } while (b == 255);
      }
      if (len < 0) return false;
      const bool cbr = (ch & 0x80) == 0;
      if (!cbr) {
        last = len;
        for (uint32_t i = 0; i + 1 < count; ++i) {
          int32_t s = 0;
          const int32_t bytes = parseSize(d, len, &s);
          if (bytes < 0) return false;
          len -= bytes;
          if (s > len) return false;
          d += bytes;
          out->size[i] = static_cast<uint16_t>(s);
          last -= bytes + s;
        }
        if (last < 0) return false;
      } else {
        last = len / static_cast<int32_t>(count);
        if (last * static_cast<int32_t>(count) != len) return false;
        for (uint32_t i = 0; i + 1 < count; ++i) out->size[i] = static_cast<uint16_t>(last);
      }
      break;
    }
  }
  if (last > static_cast<int32_t>(kMaxFrameBytes)) return false;
  out->size[count - 1] = static_cast<uint16_t>(last);
  out->payload = d;
  out->toc = toc;
  out->count = static_cast<uint8_t>(count);
  return true;
}

uint32_t framePacket(const Frames& f, uint32_t i, uint8_t* out) {
  if (i >= f.count) return 0;
  out[0] = static_cast<uint8_t>(f.toc & 0xFC);
  std::memcpy(out + 1, f.frame(i), f.size[i]);
  return 1u + f.size[i];
}

// ---- the anchor ----

ResumeAnchor makeAnchor(uint64_t sample, uint32_t fileSize, uint64_t lengthSamples) {
  ResumeAnchor a;
  a.kind = ResumeAnchor::Kind::Opus;
  a.exact = true;
  a.rate = kRate;
  a.sample = sample;
  a.fileSize = fileSize;
  a.frameHash = static_cast<uint32_t>(lengthSamples);
  return a;
}

const char* anchorCheckName(AnchorCheck c) {
  switch (c) {
    case AnchorCheck::Ok: return "ok";
    case AnchorCheck::Kind: return "the kind";
    case AnchorCheck::Size: return "the size";
    case AnchorCheck::Length: return "the length";
    case AnchorCheck::Tail: return "in its last 5 s";
  }
  return "?";
}

AnchorCheck checkAnchor(const ResumeAnchor& a, uint32_t fileSize, uint64_t lengthSamples) {
  if (a.kind != ResumeAnchor::Kind::Opus || a.rate != kRate || !a.exact) return AnchorCheck::Kind;
  if (a.fileSize != fileSize) return AnchorCheck::Size;
  // A length not known (no last page found) can't vouch for the anchor.
  if (lengthSamples == 0 || a.frameHash != static_cast<uint32_t>(lengthSamples)) return AnchorCheck::Length;
  // The tail rule (trackseek::startMs()) in samples, not ms: a sample
  // over 2^32 ms in would wrap a ms figure to something inside the track
  // and slip through as a start past the end.
  const uint64_t tail = static_cast<uint64_t>(trackseek::kTailMs) * (kRate / 1000);
  if (a.sample > 0 && (a.sample >= lengthSamples || lengthSamples - a.sample <= tail)) return AnchorCheck::Tail;
  return AnchorCheck::Ok;
}

// ---- the track ----

const char* Reader::openName(Open o) {
  switch (o) {
    case Open::Ok: return "ok";
    case Open::NotOgg: return "not Ogg";
    case Open::NotOpus: return "not Opus";
    case Open::Multiplexed: return "multiplexed";
    case Open::BadHead: return "a bad header";
    case Open::Unsupported: return "unsupported";
    case Open::NoAudio: return "no audio";
    case Open::BadStart: return "a bad start";
    case Open::ShortFrames: return "short frames";
  }
  return "?";
}

const char* Reader::endName(End e) {
  switch (e) {
    case End::None: return "not ended";
    case End::Eos: return "the end of the stream";
    case End::Truncated: return "the file's end before the stream's";
    case End::Chained: return "another stream after it";
  }
  return "?";
}

Reader::Reader(trackseek::FileReader& file, uint32_t fileSize, uint8_t* pageBuf, uint8_t* packetBuf)
    : pages_(file, fileSize, pageBuf),
      packetBuf_(packetBuf),
      size_(fileSize),
      linkEnd_(fileSize),
      scanLimit_(kResyncScan),
      scanBudget_(kResyncBudget) {}

const char* Reader::refusal(char* buf, size_t n) const {
  switch (open_) {
    case Open::Ok:
      snprintf(buf, n, "%s", "");
      break;
    case Open::NotOgg:
      snprintf(buf, n, "not an Ogg file");
      break;
    case Open::NotOpus:
      switch (codec_) {
        case Codec::Vorbis: snprintf(buf, n, "Ogg Vorbis isn't supported (only Opus)"); break;
        case Codec::Flac: snprintf(buf, n, "Ogg FLAC isn't supported (only Opus)"); break;
        case Codec::Speex: snprintf(buf, n, "Speex isn't supported (only Opus)"); break;
        case Codec::Theora: snprintf(buf, n, "an Ogg video (Theora), not Opus"); break;
        default: snprintf(buf, n, "an Ogg file, but not Opus"); break;
      }
      break;
    case Open::Multiplexed:
      snprintf(buf, n, "an Ogg file with several streams (video?) isn't supported");
      break;
    case Open::BadHead:
      snprintf(buf, n, "damaged Opus header");
      break;
    case Open::Unsupported:
      if (headCheck_ == HeadCheck::Family) {
        snprintf(buf, n, "Opus channel mapping family %u isn't supported", static_cast<unsigned>(head_.family));
      } else if (headCheck_ == HeadCheck::Mapping) {
        snprintf(buf, n, "an Opus channel mapping table this player can't follow");
      } else if (head_.channels > 2) {
        snprintf(buf, n, "surround Opus (%u channels) isn't supported", static_cast<unsigned>(head_.channels));
      } else {
        snprintf(buf, n, "Opus with %u streams for %u channels isn't supported", static_cast<unsigned>(head_.streams),
                 static_cast<unsigned>(head_.channels));
      }
      break;
    case Open::NoAudio:
      snprintf(buf, n, "no audio in it");
      break;
    case Open::BadStart:
      snprintf(buf, n, "damaged Opus start (the first audio page's granule position)");
      break;
    case Open::ShortFrames:
      snprintf(buf, n, "Opus with %s ms frames isn't supported (10 ms or longer only: too slow to decode here)",
               shortFrame_ <= 120 ? "2.5" : "5");
      break;
  }
  return buf;
}

const char* Reader::refusalNote(char* buf, size_t n) const {
  switch (open_) {
    case Open::Ok:
      snprintf(buf, n, "%s", "");
      break;
    case Open::NotOgg:
      snprintf(buf, n, "not an Ogg file");
      break;
    case Open::NotOpus:
      switch (codec_) {
        case Codec::Vorbis: snprintf(buf, n, "Ogg Vorbis isn't supported"); break;
        case Codec::Flac: snprintf(buf, n, "Ogg FLAC isn't supported"); break;
        case Codec::Speex: snprintf(buf, n, "Speex isn't supported"); break;
        case Codec::Theora: snprintf(buf, n, "an Ogg video, not Opus"); break;
        default: snprintf(buf, n, "an Ogg file, but not Opus"); break;
      }
      break;
    case Open::Multiplexed:
      snprintf(buf, n, "several Ogg streams aren't supported");
      break;
    case Open::BadHead:
      snprintf(buf, n, "damaged Opus header");
      break;
    case Open::Unsupported:
      if (headCheck_ == HeadCheck::Family) {
        snprintf(buf, n, "Opus mapping family %u isn't supported", static_cast<unsigned>(head_.family));
      } else if (headCheck_ == HeadCheck::Mapping) {
        snprintf(buf, n, "an unusual Opus channel mapping");
      } else if (head_.channels > 2) {
        snprintf(buf, n, "surround Opus isn't supported");
      } else {
        snprintf(buf, n, "an unusual Opus channel layout");
      }
      break;
    case Open::NoAudio:
      snprintf(buf, n, "no audio in it");
      break;
    case Open::BadStart:
      snprintf(buf, n, "damaged Opus start");
      break;
    case Open::ShortFrames:
      snprintf(buf, n, "Opus with %s ms frames isn't supported", shortFrame_ <= 120 ? "2.5" : "5");
      break;
  }
  return buf;
}

void Reader::clearOpen() {
  open_ = Open::NotOgg;
  head_ = Head{};
  headCheck_ = HeadCheck::Ok;
  codec_ = Codec::None;
  shortFrame_ = 0;
  serial_ = 0;
  headCrc_ = 0;
  firstAudio_ = 0;
  g0_ = 0;
  firstGranuleAt_ = 0;
  firstGranuleEnd_ = 0;
  firstGranuleSeq_ = 0;
  firstGranuleContinues_ = false;
  firstGranule_ = -1;
  tagsBytes_ = 0;
  tagsPages_ = 0;
  lastGranule_ = -1;
  lastPageAt_ = 0;
  chained_ = false;
  linkEnd_ = size_;
  stats_ = Stats{};
  loaded_ = false;
  end_ = End::None;
}

Reader::Open Reader::open(bool withTail) {
  // The headers and the first audio page are read whole whatever the
  // slice (the open is synchronous, as the tail scan and the plans are: a
  // 16 KB page is two reads, not three, a 64 KB one two, not nine), and
  // the first audio page stays in hand for the first next().
  const uint32_t slice = slice_;
  slice_ = 0;
  const Open o = openPages(withTail);
  slice_ = slice;
  return o;
}

Reader::Open Reader::openPages(bool withTail) {
  clearOpen();

  // The BOS page: the stream's first packet names the codec. A page there
  // whose CRC fails is a damaged file, not another kind of file.
  ogg::Page pg;
  const ogg::PageReader::Read bos = pages_.read(0, &pg);
  if (bos == ogg::PageReader::Read::BadCrc) return open_ = Open::BadHead;
  if (bos != ogg::PageReader::Read::Ok || !pg.h.bos()) return open_ = Open::NotOgg;
  serial_ = pg.h.serial;
  headCrc_ = pg.h.crc;
  uint32_t first = 0;  // the first packet's bytes, when it completes on the page
  for (uint32_t i = 0, len = 0; i < pg.h.segments; ++i) {
    len += pg.lacing[i];
    if (pg.lacing[i] < 255) {
      first = len;
      break;
    }
  }
  codec_ = codecOf(pg.body, first);
  if (codec_ == Codec::Opus) headCheck_ = parseHead(pg.body, first, &head_);
  uint32_t off = pg.end();
  // Another BOS page right after: a grouped file (video with its audio).
  // The header read here is the OpusTags page's, used again below (one
  // read, not two).
  ogg::Header h;
  bool haveHeader = pages_.readHeader(off, &h) == ogg::PageReader::Read::Ok;
  if (haveHeader && h.bos()) return open_ = Open::Multiplexed;
  if (codec_ != Codec::Opus) return open_ = Open::NotOpus;
  if (headCheck_ != HeadCheck::Ok) {
    const bool damaged =
        headCheck_ == HeadCheck::Short || headCheck_ == HeadCheck::Version || headCheck_ == HeadCheck::Channels;
    return open_ = damaged ? Open::BadHead : Open::Unsupported;
  }

  // OpusTags, by page headers: it ends on the first page with a lacing
  // value under 255, and the first audio packet begins the page after.
  bool done = false;
  for (uint32_t i = 0; i < kMaxTagPages && !done; ++i) {
    if (!haveHeader && pages_.readHeader(off, &h) != ogg::PageReader::Read::Ok) return open_ = Open::BadHead;
    haveHeader = false;
    if (h.serial != serial_) return open_ = Open::NoAudio;  // (a grouped file was refused above)
    if (tagsPages_ == 0 && (pages_.peekBytes() < 8 || std::memcmp(pages_.peek(), "OpusTags", 8) != 0)) {
      return open_ = Open::BadHead;
    }
    ++tagsPages_;
    tagsBytes_ += h.bodyBytes;
    done = ogg::lastCompleting(pages_.lacing(), h.segments) >= 0;
    off += h.bytes();
  }
  if (!done) return open_ = Open::NoAudio;

  // The exact length first, when asked (the firmware's open): the tail
  // scan takes the page buffer, and done here the first audio page read
  // below is still in hand when the open returns, so a start from the top
  // doesn't read it twice. Its lower bound is the page after the headers
  // (the first audio page, or a damaged one before it: a BOS page at or
  // after it is a later link's either way).
  if (withTail) {
    firstAudio_ = off;
    scanTail();
  }

  // The first audio page: g0 is the first granule found less the samples
  // of the packets up to it, by their TOCs: the packets as the reader
  // hands them out (one spanning into the granule page has its TOC on
  // the page where it starts). A damaged page there is stepped over as
  // while playing (within kFirstAudioScan): the next good page of ours is
  // the start. The search is bounded as a whole too: pages that yield no
  // packet (next() says Pending) past kFirstAudioScan of file, or
  // kFirstAudioBudget of reads, are no audio.
  scanLimit_ = kFirstAudioScan;
  scanBudget_ = kFirstAudioBudget;
  reset(off);
  const uint64_t spent0 = pages_.bytesRead();
  Packet p;
  int64_t s = 0;
  int64_t granule = -1;
  for (;;) {
    const Next n = next(&p);
    if (n == Next::End) break;
    if (n == Next::Pending) {
      if (next_ - off > kFirstAudioScan || pages_.bytesRead() - spent0 > kFirstAudioBudget) break;
      continue;
    }
    if (p.samples > 0) {
      s += p.samples;
      // Frames under 10 ms (2.5 ms frames decode at 1.0x realtime on the
      // Core2: the M0 gate) are refused here, by the first audio page's
      // TOCs; a file that changes its frame size later still plays.
      const uint32_t fs = frameSamples(p.data[0]);
      if (fs < kMinFrameSamples) {
        shortFrame_ = fs;
        return open_ = Open::ShortFrames;
      }
    }
    if (p.eos && p.pageGranule < 0) return open_ = Open::BadStart;  // an EOS page without a granule
    if (p.lastOnPage && p.pageGranule >= 0) {
      granule = p.pageGranule;
      break;
    }
  }
  firstAudio_ = firstLoaded_;
  scanLimit_ = kResyncScan;
  scanBudget_ = kResyncBudget;
  if (granule < 0) return open_ = Open::NoAudio;
  if (granule > kMaxGranule) return open_ = Open::BadStart;  // 700 years in: damaged or crafted
  firstGranule_ = granule;
  firstGranuleAt_ = p.pageOffset;
  firstGranuleEnd_ = pg_.end();  // (the page in hand is the one it completed on)
  firstGranuleSeq_ = pg_.h.sequence;
  firstGranuleContinues_ = pg_.h.continues;
  g0_ = granule - s;
  if (g0_ < 0) {
    if (!p.eos) return open_ = Open::BadStart;
    g0_ = 0;  // a one-page file: the granule is its end (RFC 7845 section 4.5)
  }
  // On an EOS page the granule must reach the pre-skip, or the stream is
  // invalid (section 4.5 again: there would be nothing left to play).
  if (p.eos && granule < head_.preSkip) return open_ = Open::BadStart;
  // A stream with nothing after its pre-skip (the end's granule exactly
  // g0 + the pre-skip: an encode of an empty source) is valid and plays
  // nothing: refused as no audio, so the player skips it as it does a
  // file it can't play, instead of ending it at once and, under Repeat
  // One or All with nothing else to play, starting it again at once,
  // round and round. The end is known here from the first page when it
  // is the EOS page, else from the tail scan (open(true)); an open with
  // neither can't tell, and the player's own guard (an end with nothing
  // heard is a failure: PlaybackController::checkEnd()) stands behind.
  const int64_t end = lastGranule_ >= 0 ? lastGranule_ : (p.eos ? granule : -1);
  if (end >= g0_ && end - g0_ <= head_.preSkip) return open_ = Open::NoAudio;
  const Stats kept = stats_;
  reset(firstAudio_);  // (the first audio page stays in hand when it is the page the loop ended on)
  stats_ = kept;  // what the open stepped over counts
  stats_.pages = loaded_ ? 1 : 0;
  return open_ = Open::Ok;
}

bool Reader::openFrom(const OpenRecord& rec) {
  // A record that can't be this file's, or can't be believed (a corrupt
  // cache blob is refused by its sum before it gets here; this is the
  // last line): the size, the offsets inside the file and in order, the
  // granules in order and plausible, a head one decoder plays.
  if (rec.fileSize != size_ || rec.lastGranule < 0 || !plausible(rec.lastGranule) || !plausible(rec.firstGranule) ||
      rec.g0 < 0 || rec.firstGranule < rec.g0 || rec.lastGranule < rec.firstGranule ||
      rec.lastGranule - rec.g0 <= rec.head.preSkip ||  // nothing after the pre-skip: no open of this reader's made it
      rec.firstAudio >= size_ ||
      rec.firstGranuleAt < rec.firstAudio || rec.firstGranuleEnd <= rec.firstGranuleAt ||
      rec.firstGranuleEnd > size_ || rec.lastPageAt >= size_ || rec.linkEnd > size_ || rec.linkEnd <= rec.firstAudio ||
      rec.head.channels == 0 || rec.head.channels > 2 || rec.head.family > 1) {
    return false;
  }
  // The BOS page's header: the file's serial number and the page's CRC
  // field as the record has them (one 290-byte read; the class comment).
  ogg::Header h;
  if (pages_.readHeader(0, &h) != ogg::PageReader::Read::Ok || !h.bos() || h.serial != rec.serial ||
      h.crc != rec.headCrc) {
    return false;
  }
  clearOpen();
  open_ = Open::Ok;
  head_ = rec.head;
  codec_ = Codec::Opus;
  serial_ = rec.serial;
  headCrc_ = rec.headCrc;
  firstAudio_ = rec.firstAudio;
  g0_ = rec.g0;
  firstGranuleAt_ = rec.firstGranuleAt;
  firstGranuleEnd_ = rec.firstGranuleEnd;
  firstGranuleSeq_ = rec.firstGranuleSeq;
  firstGranuleContinues_ = rec.firstGranuleContinues;
  firstGranule_ = rec.firstGranule;
  tagsBytes_ = rec.tagsBytes;
  tagsPages_ = rec.tagsPages;
  lastGranule_ = rec.lastGranule;
  lastPageAt_ = rec.lastPageAt;
  chained_ = rec.chained;
  linkEnd_ = rec.linkEnd;
  scanLimit_ = kResyncScan;
  scanBudget_ = kResyncBudget;
  reset(firstAudio_);  // (nothing in hand: the first next() reads the page)
  return true;
}

bool Reader::record(OpenRecord* out) const {
  *out = OpenRecord{};
  if (open_ != Open::Ok || lastGranule_ < 0) return false;
  out->head = head_;
  out->fileSize = size_;
  out->serial = serial_;
  out->headCrc = headCrc_;
  out->firstAudio = firstAudio_;
  out->firstGranuleAt = firstGranuleAt_;
  out->firstGranuleEnd = firstGranuleEnd_;
  out->firstGranuleSeq = firstGranuleSeq_;
  out->firstGranuleContinues = firstGranuleContinues_;
  out->chained = chained_;
  out->g0 = g0_;
  out->firstGranule = firstGranule_;
  out->lastGranule = lastGranule_;
  out->lastPageAt = lastPageAt_;
  out->linkEnd = linkEnd_;
  out->tagsBytes = tagsBytes_;
  out->tagsPages = tagsPages_;
  return true;
}

void Reader::takePage() {
  lastComplete_ = ogg::lastCompleting(pg_.lacing, pg_.h.segments);
  seg_ = 0;
  bodyAt_ = 0;
  runStart_ = 0;
  runSeg_ = 0;
  runLen_ = 0;
  next_ = pg_.end();
}

void Reader::reset(uint32_t offset) {
  // The page in hand is the one the walk starts at (the open's first
  // audio page, a plan's Q read whole by checkProbe()): kept, taken as
  // advance() takes a page it has just read, with nothing of a packet
  // before it in hand (a packet continued onto it from the page before is
  // dropped, as after a lost page). Anything else in the buffer is gone.
  const bool keep = loaded_ && pg_.offset == offset;
  next_ = offset;
  loaded_ = false;
  lastComplete_ = -1;
  seg_ = 0;
  bodyAt_ = 0;
  runStart_ = 0;
  runSeg_ = 0;
  runLen_ = 0;
  partial_ = 0;
  dropping_ = false;
  haveSeq_ = false;
  seq_ = 0;
  gap_ = false;
  skipping_ = false;
  skipPage_ = 0;
  end_ = End::None;
  resync_ = ogg::PageReader::Scan{};  // a scan under way is dropped with the position
  if (keep) {
    loaded_ = true;
    takePage();
    seq_ = pg_.h.sequence;
    haveSeq_ = true;
    firstLoaded_ = offset;
    dropping_ = pg_.h.continued();
    ++stats_.pages;  // (counted as a page read whole, which a start without it in hand would make)
  }
}

void Reader::dropPage() {
  // The page buffer is taken for something else (a tail scan, a plan).
  // Nothing of the page in hand handed over yet (the open's first audio
  // page, a plan's Q): the walk goes back to its start, and next() reads
  // it again when it comes. Part of the way through it: the rest of the
  // page is gone with the buffer (the scan and the plans are for before
  // the first next(), or after restart() and startAt(), which reset the
  // walk anyway).
  if (!loaded_) return;
  loaded_ = false;
  if (seg_ == 0 && partial_ == 0 && runLen_ == 0) {
    // As if it had never been read: the walk starts at it again, the
    // continuity check with it (its own sequence number would read as a
    // gap otherwise), and it isn't counted twice.
    next_ = pg_.offset;
    haveSeq_ = false;
    seq_ = 0;
    dropping_ = false;
    if (stats_.pages > 0) --stats_.pages;
  }
}

void Reader::restart() { reset(firstAudio_); }

void Reader::startAt(const StartPlan& plan) {
  if (plan.fromTop) {
    restart();
    return;
  }
  reset(plan.pageOffset);
  skipping_ = plan.skipPage;
  skipPage_ = plan.pageOffset;
  // Reading starts after Q: the first page read must follow Q's sequence
  // number, or a page was lost right after Q and the first packet handed
  // over starts later than granule(Q) says (a gap, sized as any other).
  // When Q itself is read (skipPage) its sequence seeds the check.
  if (!plan.skipPage) {
    seq_ = plan.sequence;
    haveSeq_ = true;
  }
}

Reader::Step Reader::advance() {
  ogg::Page p;
  bool lostSync = false;
  loaded_ = false;  // the buffer is the next page's (or the scan's) from here
  if (resync_.active) {
    // The scan for the next good page of ours after a damaged one (begun
    // below) goes on, one read a call: a chunk of the file, or a slice of
    // a candidate page that reaches past its chunk. next() says Pending
    // between the steps, so the decode task's pass can end there as it
    // does between a page's slices (the class comment; docs/OPUS.md
    // section 8.11). None: nothing of ours within scanLimit_ bytes, or
    // scanBudget_ bytes of reads: the track ends here (a gap too long to
    // bridge).
    const ogg::PageReader::Found f = pages_.findStep(&resync_, &p, slice_);
    if (f == ogg::PageReader::Found::More) return Step::Partial;
    if (f == ogg::PageReader::Found::None) {
      end_ = End::Truncated;
      return Step::End;
    }
    ++stats_.resyncs;
    stats_.resyncBytes += p.offset - next_;
    lostSync = true;
  } else {
    if (next_ >= size_) {
      end_ = End::Truncated;
      return Step::End;
    }
    const ogg::PageReader::Read r = pages_.read(next_, &p, slice_);
    if (r == ogg::PageReader::Read::Partial) return Step::Partial;  // (a slice of it: the next call reads on)
    if (r != ogg::PageReader::Read::Ok) {
      // Damaged, junk, or cut by the file's end: the next good page of
      // ours is looked for from the byte after, the steps above (this
      // call's read, the slice that found the damage out, was its step).
      ++stats_.badPages;
      pages_.beginScan(next_ + 1, scanLimit_, serial_, false, &resync_, ogg::PageReader::kScanChunk, scanBudget_);
      return Step::Partial;
    }
  }
  next_ = p.end();
  if (p.h.serial != serial_) {
    if (p.h.bos()) {
      end_ = End::Chained;
      if (p.offset < linkEnd_) linkEnd_ = p.offset;
      return Step::End;
    }
    ++stats_.foreignPages;
    return Step::Skipped;  // (one page a call: next() counts it)
  }
  if (p.h.bos() && haveSeq_) {
    end_ = End::Chained;  // a new stream under our serial
    if (p.offset < linkEnd_) linkEnd_ = p.offset;
    return Step::End;
  }
  // Continuity (RFC 7845 section 3): a packet cut by a lost or damaged
  // page is dropped, never decoded: a continued page whose packet's
  // start we don't hold drops the rest of it; a page not continued
  // while a packet is pending drops the pending part.
  const bool gap = haveSeq_ && p.h.sequence != seq_ + 1;
  if (gap) ++stats_.sequenceGaps;
  if (gap || lostSync) gap_ = true;
  if (!haveSeq_) firstLoaded_ = p.offset;
  dropping_ = false;
  if (p.h.continued()) {
    if (gap || lostSync || partial_ == 0) {
      if (partial_ > 0) ++stats_.droppedPartials;
      partial_ = 0;
      dropping_ = true;
    }
  } else if (partial_ > 0) {
    partial_ = 0;
    ++stats_.droppedPartials;
  }
  seq_ = p.h.sequence;
  haveSeq_ = true;
  pg_ = p;
  loaded_ = true;
  takePage();
  ++stats_.pages;
  // With a slice set (the generator's reads) the page's arrival is a step
  // of its own: next() says Pending once more and hands the first packet
  // on the next call, so the decode doesn't follow the last slice's read
  // and the CRC in the same step (the class comment; docs/OPUS.md section
  // 10: the device measured 19-29 ms for the two together, over the 15 ms
  // pass budget, on every file).
  return slice_ > 0 ? Step::Partial : Step::Page;
}

int64_t Reader::samplesAfter(uint32_t seg, uint32_t bodyAt) const {
  int64_t total = 0;
  uint32_t at = bodyAt;
  uint32_t len = 0;
  for (uint32_t j = seg; j < pg_.h.segments; ++j) {
    len += pg_.lacing[j];
    if (pg_.lacing[j] < 255) {
      const int32_t n = packetSamples(pg_.body + at, len);
      if (n < 0) return -1;
      total += n;
      at += len;
      len = 0;
    }
  }
  return total;  // (a run left open at the page's end completes elsewhere: not this page's)
}

Reader::Next Reader::next(Packet* out) {
  if (end_ != End::None) return Next::End;
  // This call's share of pages that yield nothing (kNextPages,
  // kNextBudget): past it, Pending, and the next call goes on from the
  // page in hand.
  const uint64_t spent0 = pages_.bytesRead();
  uint32_t advanced = 0;
  for (;;) {
    if (!loaded_ || seg_ >= pg_.h.segments) {
      if (loaded_ && pg_.h.eos()) {
        end_ = End::Eos;
        return Next::End;
      }
      if (advanced >= kNextPages || pages_.bytesRead() - spent0 >= kNextBudget) return Next::Pending;
      const Step s = advance();
      if (s == Step::End) return Next::End;
      if (s == Step::Partial) return Next::Pending;  // a slice of the next page: the caller's pass may end here
      ++advanced;  // (a page of ours loaded, or one of another stream stepped over)
      continue;
    }
    const uint32_t i = seg_++;
    const uint8_t lv = pg_.lacing[i];
    const bool complete = lv < 255;
    if (dropping_) {
      bodyAt_ += lv;
      if (complete) {
        dropping_ = false;
        ++stats_.droppedPartials;
      }
      continue;
    }
    // The packet's bytes on this page are one run, runStart_ to bodyAt_.
    if (runLen_ == 0) {
      runStart_ = bodyAt_;
      runSeg_ = i;
    }
    bodyAt_ += lv;
    runLen_ += lv;
    if (partial_ + runLen_ > kMaxPacketBytes) {
      ++stats_.oversized;
      partial_ = 0;
      runLen_ = 0;
      dropping_ = !complete;
      continue;
    }
    if (!complete) {
      if (seg_ >= pg_.h.segments) {
        // The page ends inside the packet: its run is carried over.
        std::memcpy(packetBuf_ + partial_, pg_.body + runStart_, runLen_);
        partial_ += runLen_;
        runLen_ = 0;
      }
      continue;
    }
    if (skipping_ && pg_.offset == skipPage_) {
      // A plan's first page: its own packets end before the start wanted
      // (the one continuing out of it is the first handed over).
      partial_ = 0;
      runLen_ = 0;
      continue;
    }
    if (partial_ == 0) {
      out->data = pg_.body + runStart_;  // in place, however many segments
      out->bytes = runLen_;
    } else {
      std::memcpy(packetBuf_ + partial_, pg_.body + runStart_, runLen_);
      out->data = packetBuf_;
      out->bytes = partial_ + runLen_;
    }
    partial_ = 0;
    runLen_ = 0;
    out->samples = packetSamples(out->data, out->bytes);
    out->pageOffset = pg_.offset;
    out->pageGranule = pg_.h.granule;
    out->lastOnPage = static_cast<int32_t>(i) == lastComplete_;
    out->eos = pg_.h.eos();
    out->gapBefore = gap_;
    out->startK = -1;
    if (gap_) {
      // The gap plan: where this packet starts, by the page's granule
      // (the end of its last completing packet) less the samples of the
      // packets completing on it from this one on. This packet's own
      // samples are from its assembled bytes (it may have begun on a page
      // before this one, after a gap page that only held its head).
      const int64_t after = out->samples < 0 ? -1 : samplesAfter(i + 1, bodyAt_);
      if (after >= 0 && plausible(pg_.h.granule)) {
        out->startK = pg_.h.granule - out->samples - after;
        if (out->startK < 0) out->startK = -1;
        if (out->startK >= 0) ++stats_.gapsSized;
      }
    }
    gap_ = false;
    return Next::Packet;
  }
}

// ---- the tail scan and the probes ----

Reader::Found Reader::probeOurs(uint32_t from, uint32_t stop, bool atPage, bool needGranule, uint32_t chunk,
                                uint64_t budget, int64_t floorGranule, Probe* out) {
  if (stop > size_) stop = size_;
  if (from >= stop) return Found::None;
  const uint64_t spent0 = pages_.bytesRead();
  ogg::Header h;
  uint32_t at = from;
  bool got = false;
  if (atPage) {
    // A page starts at `from` (a step from the one before): its header
    // from the chunk in hand when a probe's scan left it there whole,
    // else one small read.
    got = (pages_.headerInChunk(from, &h) || pages_.readHeader(from, &h) == ogg::PageReader::Read::Ok) &&
          static_cast<uint64_t>(from) + h.bytes() <= size_;
  }
  if (!got) {
    // A bisection's guess (never a page's start: no header read there), or
    // junk where a page was expected: the first page header from `from`
    // on, by chunks.
    const uint32_t scanFrom = atPage ? from + 1 : from;
    if (!pages_.findHeader(scanFrom, stop - scanFrom, &h, &at, chunk, budget)) return Found::None;
  }
  for (uint32_t steps = 0;; ++steps) {
    if (at >= stop) return Found::None;
    // Any BOS page past our audio is another link's (ours is at 0), once
    // read whole and checked: a page of ours whose flag byte is damaged
    // would otherwise end the link there for every later probe and plan.
    // One whose CRC fails is damage, stepped over as junk is.
    bool damaged = false;
    if (h.bos()) {
      ogg::Page pg;
      if (pages_.read(at, &pg) == ogg::PageReader::Read::Ok) {
        out->at = at;
        out->end = at + h.bytes();
        out->granule = -1;
        out->sequence = h.sequence;
        out->continues = false;
        return Found::LinkEnd;
      }
      damaged = true;
    }
    // A granule under `floorGranule` (the probe before this one along the
    // file: granules never go down along a stream) is a damaged field or a
    // later link's page under our serial. Read whole: the CRC tells which;
    // a damaged page is stepped over, a real one handed back (the caller
    // ends its range at it).
    if (!damaged && h.serial == serial_ && plausible(h.granule) && h.granule < floorGranule) {
      ogg::Page pg;
      damaged = pages_.read(at, &pg) != ogg::PageReader::Read::Ok;
    }
    if (!damaged && h.serial == serial_ && (!needGranule || plausible(h.granule))) {
      out->at = at;
      out->end = at + h.bytes();
      out->granule = plausible(h.granule) ? h.granule : -1;
      out->sequence = h.sequence;
      out->continues = h.continues;
      return Found::Page;
    }
    const uint64_t spent = pages_.bytesRead() - spent0;
    if (steps >= kProbeWalk || spent >= budget) return Found::None;
    const uint32_t nextAt = at + h.bytes();
    if (nextAt >= stop) return Found::None;
    if (pages_.readHeader(nextAt, &h) == ogg::PageReader::Read::Ok &&
        static_cast<uint64_t>(nextAt) + h.bytes() <= size_) {
      at = nextAt;
      continue;
    }
    // Junk, or a page cut by the file's end: scan on past it.
    if (!pages_.findHeader(nextAt + 1, stop - nextAt - 1, &h, &at, chunk, budget - spent)) return Found::None;
  }
}

bool Reader::checkProbe(const Probe& p) {
  // Read whole into the page buffer; right, it is the page in hand, so a
  // startAt() to it (reset(): the page kept) reads nothing.
  loaded_ = false;
  ogg::Page pg;
  const bool ok =
      pages_.read(p.at, &pg) == ogg::PageReader::Read::Ok && pg.h.serial == serial_ && pg.h.granule == p.granule;
  if (ok) {
    pg_ = pg;
    loaded_ = true;
  }
  return ok;
}

bool Reader::walkTail(uint32_t from, uint32_t end, uint32_t chunk, uint64_t budget) {
  if (end > size_) end = size_;
  if (from >= end) return false;
  const uint64_t spent0 = pages_.bytesRead();
  ogg::Header h;
  uint32_t at = 0;
  if (!pages_.findHeader(from, end - from, &h, &at, chunk, budget)) return false;
  // The pages from there to `end`, by their headers: the last one of ours
  // with a granule (one that can be believed), and the one before it in
  // case its CRC fails. The headers are parsed from the chunk of the file
  // in the page buffer (findHeader's, then one read here per chunk of
  // pages), not read one by one: pages of a few dozen bytes (one tiny
  // packet each, as a muxer flushing every packet writes them) would cost
  // a 290-byte read per page and run the budget out before the end.
  int64_t best = -1, before = -1;
  uint32_t bestAt = 0, beforeAt = 0;
  for (uint32_t i = 0; i < 4096; ++i) {
    if (h.bos() && at >= firstAudio_) {
      // A link after the first, if the page is one (read whole and
      // checked: a damaged flag on a page of ours isn't a link, and such
      // a page is passed over). Everything after a real BOS page is the
      // later link's, whatever serial it carries: the walk ends here.
      ogg::Page pg;
      if (pages_.read(at, &pg) == ogg::PageReader::Read::Ok) {
        chained_ = true;
        if (at < linkEnd_) linkEnd_ = at;
        break;
      }
    } else if (h.serial == serial_ && !h.bos() && plausible(h.granule)) {
      before = best;
      beforeAt = bestAt;
      best = h.granule;
      bestAt = at;
    }
    const uint32_t nextAt = at + h.bytes();
    if (nextAt >= end) break;
    const uint64_t spent = pages_.bytesRead() - spent0;
    if (spent >= budget) break;  // a tail of junk: what was walked decides
    // The next header from the chunk in hand; a fresh chunk at it when it
    // isn't there whole (a page read took the buffer, or the chunk ends
    // inside it).
    bool got = pages_.headerInChunk(nextAt, &h);
    if (!got && static_cast<uint64_t>(nextAt) + ogg::kMaxHeaderBytes > pages_.chunkEnd()) {
      pages_.readChunk(nextAt, chunk);
      got = pages_.headerInChunk(nextAt, &h);
    }
    if (got && static_cast<uint64_t>(nextAt) + h.bytes() <= size_) {
      at = nextAt;
      continue;
    }
    // Junk, or a page cut by the file's end: scan on past it, through
    // the chunk in hand first (a tail of crafted headers would otherwise
    // cost a chunk's read per junk byte), then the file.
    uint32_t scanFrom = nextAt + 1;
    if (pages_.findHeaderInChunk(scanFrom, end, &h, &at)) continue;
    scanFrom = at > scanFrom ? at : scanFrom;
    if (scanFrom >= end) break;
    if (!pages_.findHeader(scanFrom, end - scanFrom, &h, &at, chunk, budget - spent)) break;
  }
  // The page taken, checked whole (its CRC, its granule as the header
  // said): in memory when it lies in the chunk whole (a window read as
  // one chunk holds the file's last page: no read), else read whole.
  ogg::Page pg;
  auto checked = [&](uint32_t pageAt, int64_t granule) -> bool {
    ogg::Header hh;
    if (pages_.headerInChunk(pageAt, &hh) && static_cast<uint64_t>(pageAt) + hh.bytes() <= pages_.chunkEnd()) {
      return pages_.pageInChunk(pageAt, &pg) && pg.h.granule == granule;
    }
    return pages_.read(pageAt, &pg) == ogg::PageReader::Read::Ok && pg.h.granule == granule;
  };
  if (best >= 0 && checked(bestAt, best)) {
    lastGranule_ = best;
    lastPageAt_ = bestAt;
    return true;
  }
  if (before >= 0 && checked(beforeAt, before)) {
    lastGranule_ = before;
    lastPageAt_ = beforeAt;
    return true;
  }
  return false;
}

bool Reader::scanTail() {
  lastGranule_ = -1;
  lastPageAt_ = 0;
  chained_ = false;
  linkEnd_ = size_;
  dropPage();  // the page buffer is taken
  // The windows at the file's end (the constants' comment): the first two
  // read as one chunk each, the third in 16 KB chunks.
  struct Window {
    uint32_t bytes;
    uint32_t chunk;
  };
  const Window windows[3] = {{kTailShort, kTailShort}, {kTailMid, kTailMid}, {kTailLong, kTailChunk}};
  uint32_t lastFrom = 0xFFFFFFFFu;
  for (const Window& w : windows) {
    uint32_t from = size_ > w.bytes ? size_ - w.bytes : 0;
    if (from < firstAudio_) from = firstAudio_;
    if (from == lastFrom) return false;  // the window before reached the first audio page already
    lastFrom = from;
    // What this window's walk may read in all (the chunk, a header per
    // page, a chunk more per stretch of junk).
    if (walkTail(from, size_, w.chunk, static_cast<uint64_t>(kTailBudgetMul) * w.bytes + w.chunk)) return true;
  }
  // Nothing of ours in the last 73 KB: a chained file (another link's
  // pages fill the tail), or a junk tail longer than the window. Where
  // our link ends, by bisection on the serial numbers: a page of ours
  // starts at `lo`, none at or after `hi` (a BOS page there, junk, or the
  // file's end), the links being one after another (RFC 3533). Then the
  // window before `hi`.
  uint32_t lo = firstAudio_;
  uint32_t hi = size_;
  for (uint32_t probes = 0; probes < kMaxProbes && hi - lo > kTailLong; ++probes) {
    const uint32_t mid = lo + (hi - lo) / 2;
    Probe p;
    const Found r = probeOurs(mid, hi, false, false, kTailChunk, kProbeBudget, -1, &p);
    if (r == Found::Page) {
      lo = p.at;
    } else if (r == Found::LinkEnd) {
      hi = p.at;
      chained_ = true;
    } else {
      hi = mid;
    }
  }
  if (chained_ && hi < linkEnd_) linkEnd_ = hi;
  const uint32_t w = hi - lo;
  return walkTail(lo, hi, kTailChunk, static_cast<uint64_t>(kTailBudgetMul) * w + kTailChunk);
}

uint64_t Reader::lengthSamples() const {
  if (lastGranule_ < 0) return 0;
  const int64_t n = lastGranule_ - g0_ - head_.preSkip;
  return n > 0 ? static_cast<uint64_t>(n) : 0;
}

uint32_t Reader::lengthMs() const { return static_cast<uint32_t>(lengthSamples() * 1000 / kRate); }

// ---- the start plan ----

void Reader::planStart(uint64_t target, uint32_t prerollSamples, StartPlan* out) {
  *out = StartPlan{};
  out->target = target;
  out->fromTop = true;
  out->pageOffset = firstAudio_;
  out->k = g0_;
  out->decodeFrom = g0_;
  // At or past the end (the length known), or past any granule a file can
  // hold: the plain start, target 0 (the tail rule has said so already
  // for a start asked in ms; a plan would only land on the last page and
  // end at once, and keeping from G past the end would decode the whole
  // track and keep nothing).
  const bool pastEnd = target > static_cast<uint64_t>(kMaxGranule) ||
                       (lastGranule_ >= 0 && static_cast<int64_t>(target) + g0_ + head_.preSkip >= lastGranule_);
  if (pastEnd) target = 0;
  out->target = target;
  const int64_t G = static_cast<int64_t>(target) + g0_ + head_.preSkip;
  out->keepFrom = G;
  const int64_t P = G - static_cast<int64_t>(prerollSamples);
  // Inside the first page, or the first preroll: from the top with the
  // pre-skip (RFC 7845 section 4.6).
  if (pastEnd || firstGranule_ < 0 || P < firstGranule_) return;
  dropPage();  // the page buffer is taken
  const uint32_t reads0 = pages_.reads();
  const uint64_t spent0 = pages_.bytesRead();
  // The last page Q with granule(Q) <= P: `lo` has one at or under P,
  // `hi` is where a page over P (or the link's end, or junk) starts: the
  // last page itself when the tail scan found it (its granule is over P,
  // or the target would be past the end, above), so the interpolation's
  // upper point is exact from the first probe. Interpolated by granule
  // between the two, a plain halving every third probe so a file whose
  // bytes don't follow its time can't make it crawl. `prev` is the lo
  // before the current one, in case Q fails its check below. The whole
  // plan, the walk after the bisection included, reads at most
  // kPlanBudget (each probe gets what is left of it, up to its own).
  Probe lo;
  lo.at = firstGranuleAt_;
  lo.end = firstGranuleEnd_;
  lo.granule = firstGranule_;
  lo.sequence = firstGranuleSeq_;
  lo.continues = firstGranuleContinues_;
  const Probe first = lo;
  Probe prev = lo;
  uint32_t hi = linkEnd_;
  int64_t hiG = lastGranule_ > lo.granule ? lastGranule_ + 1 : -1;
  if (lastGranule_ > lo.granule && lastPageAt_ >= lo.end && lastPageAt_ < linkEnd_) {
    hi = lastPageAt_;
    hiG = lastGranule_;
  }
  auto left = [&]() -> uint64_t {
    const uint64_t spent = pages_.bytesRead() - spent0;
    const uint64_t rest = spent < kPlanBudget ? kPlanBudget - spent : 0;
    return rest < kProbeBudget ? rest : kProbeBudget;
  };
  for (uint32_t it = 0; it < kMaxProbes; ++it) {
    if (lo.end >= hi) break;
    if (left() == 0) break;
    uint64_t guess;
    if (hiG > lo.granule && it % 3 != 2) {
      const uint64_t span = hi - lo.end;
      const uint64_t want = static_cast<uint64_t>(P - lo.granule);
      const uint64_t range = static_cast<uint64_t>(hiG - lo.granule);
      // (want / range of the span, in double: the span is under 4 GB and
      // the granules under 2^40; a rounding error only moves the guess)
      guess = lo.end + static_cast<uint64_t>((static_cast<double>(span) * static_cast<double>(want)) /
                                             static_cast<double>(range));
      guess = guess > lo.end + kAimEarly ? guess - kAimEarly : lo.end;
    } else {
      guess = lo.end + (hi - lo.end) / 2;
    }
    if (guess >= hi) guess = hi - 1;
    if (guess < lo.end) guess = lo.end;
    ++out->probes;
    Probe p;
    const Found r = probeOurs(static_cast<uint32_t>(guess), hi, false, true, kSeekChunk, left(), lo.granule, &p);
    if (r != Found::Page) {
      // Nothing of ours between the guess and hi (another link's pages,
      // junk, the file's end): the page wanted is before the guess.
      if (r == Found::LinkEnd) {
        if (p.at < linkEnd_) linkEnd_ = p.at;
        chained_ = true;
      }
      if (guess == lo.end) break;
      hi = r == Found::LinkEnd ? p.at : static_cast<uint32_t>(guess);
      continue;
    }
    // A page past lo with a lower granule is a later link's under our
    // serial (the probe checked it whole: damage was stepped over), and the
    // page wanted is before it.
    if (p.granule <= P && p.granule >= lo.granule) {
      prev = lo;
      lo = p;
    } else {
      hi = p.at;
      hiG = p.granule;
    }
  }
  // Forward from lo by headers to the last page at or under P (the
  // probes stop a page or two short; a crawl stops at its bound, the
  // plan's budget included).
  for (uint32_t i = 0; i < kProbeWalk && left() > 0; ++i) {
    Probe n;
    if (probeOurs(lo.end, hi, true, true, kSeekChunk, left(), lo.granule, &n) != Found::Page || n.granule > P ||
        n.granule < lo.granule) {
      break;
    }
    prev = lo;
    lo = n;
  }
  // Q read whole and checked before it is believed: the probes only saw
  // headers, and a damaged granule field (its CRC wrong) would land the
  // start wrong. The page before it when it fails, and the first audio
  // page (checked by the open) past that: earlier, still exact. The page
  // checked stays in hand (checkProbe()), so a startAt() to it (a packet
  // continuing out of Q: Q itself is where reading starts) reads nothing;
  // a start at the page after Q reads that one.
  if (lo.at != first.at && !checkProbe(lo)) lo = prev.at != first.at && checkProbe(prev) ? prev : first;
  out->fromTop = false;
  out->k = lo.granule;
  out->decodeFrom = P;
  out->sequence = lo.sequence;
  // Reading starts after Q; at Q itself, its own packets stepped over,
  // when a packet continues out of it (that one is the first wanted, and
  // its head is on Q).
  out->pageOffset = lo.continues ? lo.at : lo.end;
  out->skipPage = lo.continues;
  out->reads = pages_.reads() - reads0;
  out->bytes = pages_.bytesRead() - spent0;
}

uint32_t Reader::planStartMs(uint32_t ms, uint32_t prerollMs, StartPlan* out) {
  const uint32_t land = trackseek::startMs(ms, lengthMs());
  planStart(static_cast<uint64_t>(land) * (kRate / 1000), prerollMs * (kRate / 1000), out);
  return land;
}

// ---- the timeline ----

void Timeline::start(int64_t g0, uint32_t preSkip) { start(g0, g0, g0 + preSkip, g0 + preSkip); }

void Timeline::start(int64_t k, int64_t decodeFrom, int64_t keepFrom, int64_t origin) {
  k_ = k;
  keepFrom_ = keepFrom;
  decodeFrom_ = decodeFrom;
  origin_ = origin;
  keepTo_ = -1;
  dropTo_ = keepFrom;
  prevGranule_ = k;
  pageStartK_ = k;
  gap_ = 0;
  curPage_ = 0;
  havePage_ = false;
  lastOnPage_ = false;
  eos_ = false;
  pageGranule_ = -1;
  kept_ = 0;
  corrections_ = 0;
  slip_ = 0;
}

void Timeline::packet(const Reader::Packet& p) {
  // A granule past kMaxGranule is damaged or crafted: as none.
  const int64_t granule = plausible(p.pageGranule) ? p.pageGranule : -1;
  // The gap plan: the fill owed before this packet (the count is behind
  // where the file says the packet starts). A count ahead of it (a page
  // whose granule wasn't believed, say) is put right at the page's end.
  int64_t owed = p.gapBefore && p.startK >= 0 && p.startK > k_ ? p.startK - k_ : 0;
  const bool sized = owed > 0;
  if (sized && k_ < keepFrom_) {
    // What lies before the first kept sample is never heard (a plan's
    // preroll crossing a damaged stretch, the pre-skip): the count steps
    // over it, no fill is made for it, and only what would be heard is
    // left for the generator's 10 s rule. A seek to the first good page
    // after a long damaged stretch plays; from the top the stretch still
    // ends the track.
    const int64_t step = p.startK < keepFrom_ ? owed : keepFrom_ - k_;
    k_ += step;
    owed -= step;
  }
  gap_ = owed;
  if (!havePage_ || p.pageOffset != curPage_) {
    havePage_ = true;
    curPage_ = p.pageOffset;
    pageStartK_ = k_ + gap_;
    if (p.eos && granule >= 0) {
      // The EOS page keeps granule(EOS) - granule(the page before) of the
      // samples completing on it; on a one-page file the page before is
      // the start, g0 (a trim micro-opus misses). After a gap the page
      // before is lost: the page's packets are kept to its granule (the
      // fill was sized to end where they start, so the length holds; the
      // encoder's padding plays in place of as many fill samples).
      int64_t keep = granule - prevGranule_;
      if (keep < 0) keep = 0;
      keepTo_ = sized ? granule : pageStartK_ + keep;
    }
  }
  lastOnPage_ = p.lastOnPage;
  eos_ = p.eos;
  pageGranule_ = granule;
}

Timeline::Keep Timeline::decoded(uint32_t n) {
  const int64_t lo = k_;
  const int64_t hi = k_ + n;
  k_ = hi;
  const int64_t from = lo < dropTo_ ? dropTo_ : lo;  // (dropTo_ is never under keepFrom_)
  int64_t to = hi;
  if (keepTo_ >= 0 && to > keepTo_) to = keepTo_;
  Keep r;
  if (to > from) {
    r.skip = static_cast<uint32_t>(from - lo);
    r.take = static_cast<uint32_t>(to - from);
    kept_ += r.take;
  } else {
    r.skip = n;
    r.take = 0;
  }
  return r;
}

void Timeline::packetDone() {
  if (lastOnPage_ && pageGranule_ >= 0) {
    if (!eos_ && k_ != pageGranule_) {
      slip_ += pageGranule_ - k_;
      ++corrections_;
      // Ahead of the file (the class comment): what comes next up to
      // here was heard already, as the concealment that got ahead.
      if (k_ > pageGranule_ && k_ > dropTo_) dropTo_ = k_;
      k_ = pageGranule_;
    }
    prevGranule_ = pageGranule_;
  }
  lastOnPage_ = false;
}

}  // namespace oggopus
