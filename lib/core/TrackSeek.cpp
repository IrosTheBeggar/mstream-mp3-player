// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TrackSeek.h"

#include <cmath>
#include <cstring>

#include "LameTag.h"
#include "TrackProgress.h"

namespace trackseek {

namespace {

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint32_t be16(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 8) | p[1]; }

// The first frame and what its Xing/VBRI header says (TrackProgress's
// mp3HeaderDurationMs() finds it the same way).
struct Header {
  progress::Mp3Frame f{};
  size_t at = 0;                     // the first frame, in buf
  uint32_t frames = 0;               // audio frames (0: not said)
  uint32_t bytes = 0;                // the stream's bytes from the first frame (0: not said)
  const uint8_t* xingToc = nullptr;  // 100 points, in buf
  const uint8_t* vbriToc = nullptr;  // vbriEntries of vbriEntrySize bytes, in buf
  uint32_t vbriEntries = 0, vbriScale = 0, vbriEntrySize = 0, vbriFramesPerEntry = 0;
  bool info = false;                 // "Info": LAME's header for a CBR file
  lametag::Info lame;                // its LAME extension (lame.lame: trusted)

  // The decoded stream's length (the header frame not counted).
  uint32_t durationMs() const {
    return static_cast<uint32_t>(static_cast<uint64_t>(frames) * static_cast<uint64_t>(f.samples) * 1000 /
                                 static_cast<uint64_t>(f.rate));
  }
};

bool findHeader(const uint8_t* buf, size_t n, Header* h) {
  for (size_t i = 0; i + 4 <= n; ++i) {
    progress::Mp3Frame f;
    if (!progress::parseMp3Frame(buf + i, &f)) continue;
    const size_t next = i + static_cast<size_t>(f.length);
    progress::Mp3Frame g;
    if (next + 4 <= n && !progress::parseMp3Frame(buf + next, &g)) continue;
    h->f = f;
    h->at = i;
    lametag::parse(buf, n, &h->lame);  // (the same first frame)
    const size_t xing = i + 4 + static_cast<size_t>(f.sideInfo);
    const size_t vbri = i + 4 + 32;
    if (xing + 8 <= n && (std::memcmp(buf + xing, "Xing", 4) == 0 || std::memcmp(buf + xing, "Info", 4) == 0)) {
      // The flags, then (each only if flagged) frames, bytes, the TOC.
      h->info = std::memcmp(buf + xing, "Info", 4) == 0;
      const uint32_t flags = be32(buf + xing + 4);
      size_t p = xing + 8;
      if (flags & 1) {
        if (p + 4 > n) return true;
        h->frames = be32(buf + p);
        p += 4;
      }
      if (flags & 2) {
        if (p + 4 > n) return true;
        h->bytes = be32(buf + p);
        p += 4;
      }
      if ((flags & 4) && p + 100 <= n) h->xingToc = buf + p;
    } else if (vbri + 26 <= n && std::memcmp(buf + vbri, "VBRI", 4) == 0) {
      // Version, delay, quality (2 bytes each), bytes, frames, then the
      // TOC's entries, scale, entry size and frames per entry.
      h->bytes = be32(buf + vbri + 10);
      h->frames = be32(buf + vbri + 14);
      const uint32_t entries = be16(buf + vbri + 18);
      const uint32_t size = be16(buf + vbri + 22);
      const uint32_t perEntry = be16(buf + vbri + 24);
      if (entries > 0 && size >= 1 && size <= 4 && perEntry > 0 && vbri + 26 + entries * size <= n) {
        h->vbriToc = buf + vbri + 26;
        h->vbriEntries = entries;
        h->vbriScale = be16(buf + vbri + 20);
        h->vbriEntrySize = size;
        h->vbriFramesPerEntry = perEntry;
      }
    }
    return true;
  }
  return false;
}

// A file without a header: VBR if the frames in buf (a chain of headers
// from the first) differ in bitrate.
bool bitratesDiffer(const uint8_t* buf, size_t n, const Header& h) {
  progress::Mp3Frame f = h.f;
  for (size_t i = h.at;;) {
    const size_t next = i + static_cast<size_t>(f.length);
    progress::Mp3Frame g;
    if (next + 4 > n || !progress::parseMp3Frame(buf + next, &g)) return false;
    if (g.kbps != f.kbps) return true;
    i = next;
    f = g;
  }
}

// A file without a header: how its length and bytes are read. The first
// frame's bitrate when the frames agree on it and a hint (if any) agrees
// with the length that gives (within kHintSlackPercent: the hint is the
// backend's read-rate estimate); else the hint as the length, the bytes
// spread evenly over it; else nothing (Unplaced).
constexpr uint32_t kHintSlackPercent = 3;

struct Plain {
  Mp3Seek how = Mp3Seek::Unplaced;
  uint32_t lengthMs = 0;
};

Plain plain(const uint8_t* buf, size_t n, const Header& h, uint32_t audio, uint32_t hintMs) {
  Plain p;
  const bool vbr = bitratesDiffer(buf, n, h);
  const uint32_t frameMs =
      static_cast<uint32_t>(static_cast<uint64_t>(audio) * 8 / static_cast<uint64_t>(h.f.kbps));
  const uint32_t diff = frameMs > hintMs ? frameMs - hintMs : hintMs - frameMs;
  const bool hintAgrees = static_cast<uint64_t>(diff) * 100 <= static_cast<uint64_t>(hintMs) * kHintSlackPercent;
  if (!vbr && (hintMs == 0 || hintAgrees)) {
    p.how = Mp3Seek::FrameBitrate;
    p.lengthMs = frameMs;
  } else if (hintMs > 0) {
    p.how = Mp3Seek::AverageBitrate;
    p.lengthMs = hintMs;
  }
  return p;
}

uint32_t vbriEntry(const Header& h, uint32_t i) {
  const uint8_t* p = h.vbriToc + i * h.vbriEntrySize;
  uint32_t v = 0;
  for (uint32_t k = 0; k < h.vbriEntrySize; ++k) v = (v << 8) | p[k];
  return v * (h.vbriScale ? h.vbriScale : 1);
}

}  // namespace

const char* mp3SeekName(Mp3Seek how) {
  switch (how) {
    case Mp3Seek::CbrInfo: return "CBR, Info header";
    case Mp3Seek::XingToc: return "Xing TOC";
    case Mp3Seek::VbriToc: return "VBRI TOC";
    case Mp3Seek::AverageBitrate: return "average bitrate";
    case Mp3Seek::FrameBitrate: return "the first frame's bitrate";
    case Mp3Seek::Unplaced: return "VBR, no table of contents, no length";
    case Mp3Seek::None: break;
  }
  return "none";
}

uint32_t mp3LengthMs(const uint8_t* buf, size_t n, uint32_t audioStart, uint32_t fileSize, uint32_t hintMs) {
  Header h;
  if (!findHeader(buf, n, &h)) return 0;
  const uint32_t first = audioStart + static_cast<uint32_t>(h.at);
  if (h.lame.lame || h.frames) {
    const uint32_t ms = h.lame.lame ? lametag::lengthMs(h.lame) : h.durationMs();  // (LAME's: the trimmed length)
    // A file shorter than its header says: the share it holds.
    if (h.bytes && fileSize > 0) return progress::truncatedMs(ms, h.bytes, fileSize > first ? fileSize - first : 0);
    return ms;
  }
  // No header: the audio bytes at the first frame's bitrate (bits per ms),
  // or the hint.
  if (fileSize <= first) return 0;
  return plain(buf, n, h, fileSize - first, hintMs).lengthMs;
}

namespace {

// Today's estimate, `decodedMs` into the decoded stream (from the first
// audio frame; an Info file without LAME's extension from its Info frame).
Mp3Seek estimateByte(const Header& h, const uint8_t* buf, size_t n, uint32_t audioStart, uint32_t fileSize,
                     uint32_t hintMs, uint32_t decodedMs, uint32_t* byte) {
  const uint32_t first = audioStart + static_cast<uint32_t>(h.at);
  const uint32_t total = h.bytes ? h.bytes : (fileSize > first ? fileSize - first : 0);
  const uint32_t dur = h.frames ? h.durationMs() : 0;
  uint64_t at = 0;  // from the first frame
  Mp3Seek how;
  if (h.info && !bitratesDiffer(buf, n, h)) {
    // LAME's CBR header: its TOC's 256ths of the bytes are up to ~0.4 s
    // off in a 4 min file; the bitrate is exact. With LAME's extension the
    // Info frame isn't decoded (gapless playback hands the decoder the
    // frame after it): from the first audio frame. Without, from the Info
    // frame's start: it decodes to a frame of silence, counted in the time.
    at = static_cast<uint64_t>(decodedMs) * static_cast<uint64_t>(h.f.kbps) / 8;
    if (h.lame.lame) at += h.lame.headerLength;
    how = Mp3Seek::CbrInfo;
  } else if (h.xingToc && dur > 0 && total > 0) {
    // Point i is where i% of the time starts, in 256ths of the bytes;
    // between two points, a straight line.
    uint64_t permille = static_cast<uint64_t>(decodedMs) * 100000 / dur;  // of 1%
    if (permille > 99999) permille = 99999;
    const uint32_t a = static_cast<uint32_t>(permille / 1000);
    const uint32_t frac = static_cast<uint32_t>(permille % 1000);
    const uint32_t fa = h.xingToc[a];
    const uint32_t fb = a < 99 ? h.xingToc[a + 1] : 256;
    const uint64_t pos = static_cast<uint64_t>(fa) * 1000 + (fb > fa ? static_cast<uint64_t>(fb - fa) * frac : 0);
    at = pos * total / 256000;
    how = Mp3Seek::XingToc;
  } else if (h.vbriToc) {
    // Entry i: the bytes of frames i * perEntry to (i + 1) * perEntry.
    const uint64_t target = static_cast<uint64_t>(decodedMs) * static_cast<uint64_t>(h.f.rate) /
                            (static_cast<uint64_t>(h.f.samples) * 1000);
    uint64_t frames = 0;
    for (uint32_t i = 0; i < h.vbriEntries; ++i) {
      const uint64_t bytes = vbriEntry(h, i);
      if (frames + h.vbriFramesPerEntry > target) {
        at += bytes * (target - frames) / h.vbriFramesPerEntry;
        break;
      }
      frames += h.vbriFramesPerEntry;
      at += bytes;
    }
    how = Mp3Seek::VbriToc;
  } else if (dur > 0 && total > 0) {
    at = static_cast<uint64_t>(total) * decodedMs / dur;
    how = Mp3Seek::AverageBitrate;
  } else {
    const Plain p = plain(buf, n, h, fileSize > first ? fileSize - first : 0, hintMs);
    how = p.how;
    if (how == Mp3Seek::Unplaced) return how;
    if (how == Mp3Seek::AverageBitrate) {
      at = static_cast<uint64_t>(total) * decodedMs / p.lengthMs;
    } else {
      at = static_cast<uint64_t>(decodedMs) * static_cast<uint64_t>(h.f.kbps) / 8;
    }
  }
  uint64_t b = first + at;
  if (fileSize > 0 && b >= fileSize) b = fileSize - 1;
  *byte = static_cast<uint32_t>(b);
  return how;
}

}  // namespace

Mp3Seek mp3SeekByte(const uint8_t* buf, size_t n, uint32_t audioStart, uint32_t fileSize, uint32_t hintMs,
                    uint32_t targetMs, uint32_t* byte) {
  Header h;
  if (!findHeader(buf, n, &h)) return Mp3Seek::None;
  // With LAME's extension the time asked for is on the trimmed timeline
  // (docs/GAPLESS.md section 4.6): in the decoded stream it is the encoder
  // delay and libmad's 529 samples later.
  return estimateByte(h, buf, n, audioStart, fileSize, hintMs, lametag::untrimmedMs(h.lame, targetMs), byte);
}

int32_t mp3FrameAt(const uint8_t* buf, size_t n) {
  for (size_t i = 0; i + 4 <= n; ++i) {
    progress::Mp3Frame f;
    if (!progress::parseMp3Frame(buf + i, &f)) continue;
    const size_t next = i + static_cast<size_t>(f.length);
    progress::Mp3Frame g;
    if (next + 4 > n || !progress::parseMp3Frame(buf + next, &g)) continue;
    if (g.version != f.version || g.rateIndex != f.rateIndex) continue;
    return static_cast<int32_t>(i);
  }
  return -1;
}

// ---- start plans ----

const char* sourceName(Source s) {
  switch (s) {
    case Source::Anchor: return "its resume anchor";
    case Source::Index: return "the run's index";
    case Source::Cbr: return "CBR";
    case Source::LameToc: return "LAME's TOC inverted";
    case Source::XingToc: return "the Xing TOC";
    case Source::VbriToc: return "the VBRI TOC";
    case Source::Bitrate: return "the bitrate";
    case Source::Average: return "the average bitrate";
    case Source::None: break;
  }
  return "none";
}

const char* noPlanName(NoPlan why) {
  switch (why) {
    case NoPlan::Tail: return "in its last 5 s or past its end";
    case NoPlan::Unplaced: return "VBR, no table of contents, no length";
    case NoPlan::NoFrame: return "no frame found to go on";
    case NoPlan::None: break;
  }
  return "";
}

const char* anchorCheckName(AnchorCheck c) {
  switch (c) {
    case AnchorCheck::Ok: return "ok";
    case AnchorCheck::Kind: return "the kind";
    case AnchorCheck::Size: return "the size";
    case AnchorCheck::Frame: return "the frame";
    case AnchorCheck::Preroll: return "the preroll";
    case AnchorCheck::Tail: return "in its last 5 s";
  }
  return "?";
}

LameBag lameBag(uint32_t frames) {
  LameBag b;
  while (frames / b.want >= 400) b.want *= 2;
  b.pos = frames / b.want;
  return b;
}

bool tocUsable(const uint8_t toc[100]) {
  for (int i = 1; i < 100; ++i) {
    if (toc[i] < toc[i - 1]) return false;
  }
  return true;
}

bool lameTocByte(const uint8_t toc[100], uint32_t frames, uint32_t audioBytes, uint32_t spf, uint64_t x,
                 uint32_t* byte) {
  if (frames == 0 || audioBytes == 0 || spf == 0) return false;
  const LameBag bag = lameBag(frames);
  // The points (frames, bytes x 512), (0, 0) first and (frames, all) last.
  // Point i is the byte share after (floor(i / 100 x pos) + 1) x want
  // frames, truncated to 1/256 by LAME: + 0.5/256 undoes it on average.
  // A point whose frames don't grow replaces the one before.
  uint32_t f[101];
  uint64_t b[101];
  int count = 0;
  auto add = [&](uint32_t frame, uint64_t bytes512) {
    if (count > 0 && frame <= f[count - 1]) {
      b[count - 1] = bytes512;
      return;
    }
    f[count] = frame;
    b[count] = bytes512;
    ++count;
  };
  add(0, 0);
  if (bag.pos > 0) {
    for (uint32_t i = 1; i < 100; ++i) {
      // In float, as LAME's Xing_seek_table() has it: i / 100.0f is
      // rounded, so its product with pos can fall just under a whole
      // number the exact i x pos / 100 is (pos 300, i 21: 62.99998, not
      // 63): the point then sits a bag step earlier.
      const float j = static_cast<float>(i) / 100.0f;
      uint32_t indx = static_cast<uint32_t>(std::floor(j * static_cast<float>(bag.pos)));
      if (indx > bag.pos - 1) indx = bag.pos - 1;
      add((indx + 1) * bag.want, (2 * static_cast<uint64_t>(toc[i]) + 1) * audioBytes);
    }
  }
  add(frames, 512 * static_cast<uint64_t>(audioBytes));
  if (count == 1) {  // (all of it at frame 0)
    *byte = 0;
    return true;
  }
  int a = 0;
  while (a + 1 < count && static_cast<uint64_t>(f[a + 1]) * spf <= x) ++a;
  if (a + 1 >= count) {  // at or past the end
    *byte = audioBytes;
    return true;
  }
  const uint64_t into = x - static_cast<uint64_t>(f[a]) * spf;
  const uint64_t span = static_cast<uint64_t>(f[a + 1] - f[a]) * spf;
  const uint64_t b512 = b[a] + (b[a + 1] > b[a] ? (b[a + 1] - b[a]) * into / span : 0);
  *byte = static_cast<uint32_t>(b512 / 512);
  return true;
}

namespace {

bool sameStream(const progress::Mp3Frame& a, const progress::Mp3Frame& b) {
  return a.version == b.version && a.rateIndex == b.rateIndex;
}

// A header at `i` whose next frame's header (the same version and rate)
// follows inside `buf`.
bool linked(const uint8_t* buf, size_t n, size_t i, progress::Mp3Frame* f) {
  if (i + 4 > n || !progress::parseMp3Frame(buf + i, f)) return false;
  const size_t next = i + static_cast<size_t>(f->length);
  progress::Mp3Frame g;
  return next + 4 <= n && progress::parseMp3Frame(buf + next, &g) && sameStream(*f, g);
}

// A chain starts at `i`: a header, the next, and the one after it (or the
// end of `buf` after the second).
bool startsChain(const uint8_t* buf, size_t n, size_t i) {
  progress::Mp3Frame f, g;
  if (!linked(buf, n, i, &f)) return false;
  const size_t next = i + static_cast<size_t>(f.length);
  progress::Mp3Frame h;
  if (!progress::parseMp3Frame(buf + next, &h)) return false;
  const size_t next2 = next + static_cast<size_t>(h.length);
  if (next2 + 4 > n) return true;
  return linked(buf, n, next, &g) && sameStream(f, g);
}

}  // namespace

Chain walkChain(const uint8_t* buf, size_t n, size_t target) {
  Chain c;
  // The chain's frames so far (a frame is at least 24 bytes: 4 KB hold
  // ~170).
  constexpr size_t kMax = 192;
  uint32_t at[kMax];
  size_t i = 0;
  while (i + 4 <= n) {
    if (!startsChain(buf, n, i)) {
      ++i;
      continue;
    }
    size_t count = 0;
    size_t p = i;
    for (;;) {
      progress::Mp3Frame f;
      if (!linked(buf, n, p, &f)) break;  // the end of `buf`, or junk
      if (count == kMax) return c;
      at[count++] = static_cast<uint32_t>(p);
      if (p >= target) {
        const size_t land = count - 1;
        const size_t h = historyFrames(static_cast<uint32_t>(f.samples));
        size_t pre = 0;  // none further back: the chain's first
        for (size_t m = land; land >= h + 1 && m-- > 0;) {
          if (land - m >= h + 1 && at[land - h] - at[m] >= kPrerollBytes) {
            pre = m;
            break;
          }
        }
        c.land = static_cast<int32_t>(at[land]);
        c.preroll = static_cast<int32_t>(at[pre]);
        c.landLength = static_cast<uint32_t>(f.length);
        return c;
      }
      p += static_cast<size_t>(f.length);
    }
    i = p + 1;  // junk (or the end): the next chain after it
  }
  return c;
}

uint32_t firstAudioByte(const uint8_t* probe, size_t n, uint32_t audioStart) {
  lametag::Info lame;
  if (!lametag::parse(probe, n, &lame)) return 0;
  return audioStart + lame.frameAt + (lame.header ? lame.headerLength : 0);
}

namespace {

// The chain walk around `estimate` (a file offset), through the file.
bool walkAt(FileReader& file, uint32_t fileSize, uint32_t firstAudio, uint32_t estimate, uint8_t* scratch,
            Plan* out) {
  if (estimate < firstAudio) estimate = firstAudio;
  const uint32_t from = estimate - firstAudio > kWalkBack ? estimate - kWalkBack : firstAudio;
  if (from >= fileSize) return false;
  const uint32_t n = file.readAt(from, scratch, kScratchBytes);
  const Chain c = walkChain(scratch, n, estimate - from);
  if (c.land < 0) return false;
  out->landByte = from + static_cast<uint32_t>(c.land);
  out->prerollByte = from + static_cast<uint32_t>(c.preroll);
  out->landLength = c.landLength;
  out->landHash = resumeanchor::frameHash(scratch + c.land, n - static_cast<uint32_t>(c.land));
  out->estimate = estimate;
  return true;
}

// CBR: frame k of a stream of frames L = num / den bytes long (LAME pads
// one byte whenever the fraction carries) starts at firstAudio + floor(k x
// L) or a byte later. The frame there, verified: a header of the stream's
// version, rate and bitrate, k by its offset, the next header after it.
bool cbrFrame(FileReader& file, uint32_t firstAudio, uint64_t num, uint64_t den, const progress::Mp3Frame& like,
              uint64_t k, uint8_t* scratch, uint32_t* byte, uint32_t* length, uint32_t* hash) {
  const uint64_t pos64 = firstAudio + k * num / den;
  if (pos64 > 0xFFFFFFFFull - kScratchBytes) return false;
  const uint32_t pos = static_cast<uint32_t>(pos64);
  const uint32_t from = pos >= firstAudio + 2 ? pos - 2 : firstAudio;
  const uint32_t n = file.readAt(from, scratch, kMaxFrameBytes + 8);
  static const int kTry[] = {0, 1, -1, 2, -2};
  for (int dd : kTry) {
    const int64_t b = static_cast<int64_t>(pos) + dd;
    if (b < static_cast<int64_t>(from)) continue;
    const size_t i = static_cast<size_t>(b - from);
    progress::Mp3Frame f;
    if (!linked(scratch, n, i, &f) || !sameStream(f, like) || f.kbps != like.kbps) continue;
    // k by its offset: round((b - firstAudio) / L).
    const uint64_t off = static_cast<uint64_t>(b) - firstAudio;
    if ((2 * off * den + num) / (2 * num) != k) continue;
    *byte = static_cast<uint32_t>(b);
    *length = static_cast<uint32_t>(f.length);
    *hash = resumeanchor::frameHash(scratch + i, n - i);
    return true;
  }
  return false;
}

bool cbrPlan(FileReader& file, const progress::Mp3Frame& f, uint32_t firstAudio, uint64_t d, uint8_t* scratch,
             Plan* out) {
  const uint64_t num = static_cast<uint64_t>(f.version == 3 ? 144000 : 72000) * static_cast<uint64_t>(f.kbps);
  const uint64_t den = static_cast<uint64_t>(f.rate);
  const uint64_t spf = static_cast<uint64_t>(f.samples);
  const uint64_t k = d / spf;
  uint32_t land = 0, length = 0, hash = 0;
  if (!cbrFrame(file, firstAudio, num, den, f, k, scratch, &land, &length, &hash)) return false;
  // The preroll: kPrerollBytes of whole frames (each at least floor(L))
  // before frame k - h, or the first audio frame.
  const uint64_t h = historyFrames(static_cast<uint32_t>(spf));
  const uint64_t floorL = num / den;
  const uint64_t back = h + (kPrerollBytes + floorL - 1) / floorL;
  uint32_t pre = firstAudio, preLength = 0, preHash = 0;
  if (k > back && !cbrFrame(file, firstAudio, num, den, f, k - back, scratch, &pre, &preLength, &preHash)) {
    return false;
  }
  out->landByte = land;
  out->landLength = length;
  out->landHash = hash;
  out->prerollByte = pre;
  out->skip = static_cast<uint32_t>(d - k * spf);
  return true;
}

}  // namespace

Plan plan(const PlanIn& in, FileReader& file, uint8_t* scratch) {
  Plan out;
  Header h;
  if (!findHeader(in.probe, in.probeBytes, &h)) {
    out.why = NoPlan::NoFrame;
    return out;
  }
  if (startMs(in.targetMs, in.lengthMs) == 0) {
    out.why = NoPlan::Tail;
    return out;
  }
  const uint32_t first = in.audioStart + static_cast<uint32_t>(h.at);
  const uint32_t firstAudio = first + (h.lame.header ? h.lame.headerLength : 0);
  out.rate = static_cast<uint32_t>(h.f.rate);
  out.spf = static_cast<uint32_t>(h.f.samples);
  // The time asked as a sample of the trimmed timeline, and of the decoded
  // stream from the first audio frame.
  const uint64_t t = static_cast<uint64_t>(in.targetMs) * out.rate / 1000;
  const uint64_t trimSkip =
      in.useTag && h.lame.lame ? static_cast<uint64_t>(h.lame.delay) + lametag::kDecoderDelay : 0;
  const uint64_t d = t + trimSkip;
  out.sample = t;

  // 3. CBR arithmetic: exact.
  const uint32_t audio = in.fileSize > first ? in.fileSize - first : 0;
  const bool cbr = (h.info || !h.lame.header) && !bitratesDiffer(in.probe, in.probeBytes, h) &&
                   (h.info || plain(in.probe, in.probeBytes, h, audio, in.hintMs).how == Mp3Seek::FrameBitrate);
  if (cbr && cbrPlan(file, h.f, firstAudio, d, scratch, &out)) {
    out.source = Source::Cbr;
    out.exact = true;
    return out;
  }
  out.skip = 0;
  // 4. LAME's TOC inverted.
  const bool lameEncoder = h.lame.lame && std::memcmp(h.lame.encoder, "LAME", 4) == 0;
  if (h.xingToc && !h.info && h.frames > 0 && h.bytes > h.lame.headerLength && lameEncoder && tocUsable(h.xingToc)) {
    uint32_t at = 0;
    if (lameTocByte(h.xingToc, h.frames, h.bytes - h.lame.headerLength, out.spf, d, &at) &&
        walkAt(file, in.fileSize, firstAudio, firstAudio + at, scratch, &out)) {
      out.source = Source::LameToc;
      return out;
    }
  }
  // 5-7: today's estimate, then the chain walk.
  uint32_t byte = 0;
  const auto decodedMs = static_cast<uint32_t>(d * 1000 / out.rate);
  const Mp3Seek how =
      estimateByte(h, in.probe, in.probeBytes, in.audioStart, in.fileSize, in.hintMs, decodedMs, &byte);
  if (how == Mp3Seek::Unplaced) {
    out.why = NoPlan::Unplaced;
    return out;
  }
  if (how == Mp3Seek::None || !walkAt(file, in.fileSize, firstAudio, byte, scratch, &out)) {
    out.why = NoPlan::NoFrame;
    return out;
  }
  switch (how) {
    case Mp3Seek::XingToc: out.source = Source::XingToc; break;
    case Mp3Seek::VbriToc: out.source = Source::VbriToc; break;
    case Mp3Seek::AverageBitrate: out.source = Source::Average; break;
    default: out.source = Source::Bitrate; break;  // (CBR whose frames weren't where arithmetic said)
  }
  return out;
}

AnchorCheck checkAnchor(const ResumeAnchor& a, FileReader& file, uint32_t fileSize, uint32_t firstAudio,
                        uint32_t lengthMs, uint8_t* scratch, Plan* out) {
  if (a.kind != ResumeAnchor::Kind::Mp3 || a.rate == 0) return AnchorCheck::Kind;
  if (a.fileSize != fileSize) return AnchorCheck::Size;
  // The landing frame: a header at its rate, its first bytes as hashed, the
  // next header after it.
  const uint32_t n = a.frameByte < fileSize ? file.readAt(a.frameByte, scratch, kMaxFrameBytes + 4) : 0;
  progress::Mp3Frame f;
  if (!linked(scratch, n, 0, &f) || static_cast<uint32_t>(f.rate) != a.rate ||
      resumeanchor::frameHash(scratch, n) != a.frameHash) {
    return AnchorCheck::Frame;
  }
  // The preroll: a frame of the same stream, before it, not too far.
  const bool atTop = a.prerollByte == a.frameByte && a.frameByte == firstAudio;
  if (a.prerollByte < firstAudio || (a.prerollByte >= a.frameByte && !atTop) ||
      a.frameByte - a.prerollByte >= kMaxPrerollSpan) {
    return AnchorCheck::Preroll;
  }
  uint8_t head[4];
  progress::Mp3Frame p;
  if (file.readAt(a.prerollByte, head, 4) != 4 || !progress::parseMp3Frame(head, &p) || !sameStream(p, f)) {
    return AnchorCheck::Preroll;
  }
  const uint32_t ms = resumeanchor::ms(a);
  if (ms > 0 && startMs(ms, lengthMs) == 0) return AnchorCheck::Tail;
  *out = Plan{};
  out->source = Source::Anchor;
  out->exact = a.exact;
  out->prerollByte = a.prerollByte;
  out->landByte = a.frameByte;
  out->landLength = static_cast<uint32_t>(f.length);
  out->landHash = a.frameHash;
  out->rate = a.rate;
  out->spf = static_cast<uint32_t>(f.samples);
  out->skip = a.skip;
  out->sample = a.sample;
  return AnchorCheck::Ok;
}

bool flacStreamInfo(const uint8_t* b, size_t n, uint32_t* rate, uint64_t* totalSamples) {
  // "fLaC", then the STREAMINFO block (see TrackProgress's flacDurationMs()).
  if (n < 26 || std::memcmp(b, "fLaC", 4) != 0 || (b[4] & 0x7F) != 0) return false;
  const uint32_t r = (static_cast<uint32_t>(b[18]) << 12) | (static_cast<uint32_t>(b[19]) << 4) | (b[20] >> 4);
  if (r == 0) return false;
  *rate = r;
  *totalSamples = (static_cast<uint64_t>(b[21] & 0x0F) << 32) | be32(b + 22);
  return true;
}

}  // namespace trackseek
