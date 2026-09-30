#include "TrackSeek.h"

#include <cstring>

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

uint32_t startMs(uint32_t requestMs, uint32_t durationMs) {
  if (durationMs == 0) return requestMs;
  if (requestMs >= durationMs || durationMs - requestMs <= kTailMs) return 0;
  return requestMs;
}

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
  if (h.frames) return h.durationMs();
  // No header: the audio bytes at the first frame's bitrate (bits per ms),
  // or the hint.
  const uint32_t first = audioStart + static_cast<uint32_t>(h.at);
  if (fileSize <= first) return 0;
  return plain(buf, n, h, fileSize - first, hintMs).lengthMs;
}

Mp3Seek mp3SeekByte(const uint8_t* buf, size_t n, uint32_t audioStart, uint32_t fileSize, uint32_t hintMs,
                    uint32_t targetMs, uint32_t* byte) {
  Header h;
  if (!findHeader(buf, n, &h)) return Mp3Seek::None;
  const uint32_t first = audioStart + static_cast<uint32_t>(h.at);
  const uint32_t total = h.bytes ? h.bytes : (fileSize > first ? fileSize - first : 0);
  const uint32_t dur = h.frames ? h.durationMs() : 0;
  uint64_t at = 0;  // from the first frame
  Mp3Seek how;
  if (h.info && !bitratesDiffer(buf, n, h)) {
    // LAME's CBR header: its TOC's 256ths of the bytes are up to ~0.4 s
    // off in a 4 min file; the bitrate is exact. From the Info frame's
    // start: it decodes to a frame of silence, counted in the time too.
    at = static_cast<uint64_t>(targetMs) * static_cast<uint64_t>(h.f.kbps) / 8;
    how = Mp3Seek::CbrInfo;
  } else if (h.xingToc && dur > 0 && total > 0) {
    // Point i is where i% of the time starts, in 256ths of the bytes;
    // between two points, a straight line.
    uint64_t permille = static_cast<uint64_t>(targetMs) * 100000 / dur;  // of 1%
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
    const uint64_t target =
        static_cast<uint64_t>(targetMs) * static_cast<uint64_t>(h.f.rate) / (static_cast<uint64_t>(h.f.samples) * 1000);
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
    at = static_cast<uint64_t>(total) * targetMs / dur;
    how = Mp3Seek::AverageBitrate;
  } else {
    const Plain p = plain(buf, n, h, fileSize > first ? fileSize - first : 0, hintMs);
    how = p.how;
    if (how == Mp3Seek::Unplaced) return how;
    if (how == Mp3Seek::AverageBitrate) {
      at = static_cast<uint64_t>(total) * targetMs / p.lengthMs;
    } else {
      at = static_cast<uint64_t>(targetMs) * static_cast<uint64_t>(h.f.kbps) / 8;
    }
  }
  uint64_t b = first + at;
  if (fileSize > 0 && b >= fileSize) b = fileSize - 1;
  *byte = static_cast<uint32_t>(b);
  return how;
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

uint32_t mp3MsLeft(const uint8_t* frame, uint32_t bytesLeft) {
  progress::Mp3Frame f;
  if (!progress::parseMp3Frame(frame, &f)) return 0;
  return static_cast<uint32_t>(static_cast<uint64_t>(bytesLeft) * 8 / static_cast<uint64_t>(f.kbps));
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
