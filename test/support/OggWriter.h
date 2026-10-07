// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// Synthetic Ogg Opus files for the reader's tests (test_ogg_opus), as
// test/support/Mp3Synth.h makes MP3 streams. Header-only, test-only.
//
// - Muxer: packets in, pages out, with the lacing table (255-byte
//   segments and the last one under 255; a packet that is a multiple of
//   255 ends with a 0), packets spanning pages with the continued flag,
//   the sequence numbers and the CRC. A page holds up to maxSegments
//   segments (255: the real thing; fewer makes packets span pages).
//   page() writes one page from explicit fields, for the odd cases (a
//   wrong CRC, a zero-segment page, a flag set wrongly).
// - opusHead(), opusTags(), opusPacket(): the packets, with every field
//   under the test's control (an odd pre-skip, a 5.1 mapping, a 2 MB
//   comment header, a 120 ms code-3 packet with padding).
// - build(): a whole file from a FileSpec, with the granules as an
//   encoder's muxer writes them (g0 for a cropped start, the EOS page's
//   trimmed by endTrim), and where everything landed.
// - Reader: a trackseek::FileReader over the bytes, counting its reads.
#include <cstdint>
#include <cstring>
#include <vector>

#include "OggOpus.h"
#include "OggPage.h"
#include "TrackSeek.h"

namespace oggwriter {

using Bytes = std::vector<uint8_t>;

inline void putLe16(Bytes& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
inline void putLe32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void putLe64(Bytes& b, uint64_t v) {
  for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// One page, every field given. The CRC is computed over the result (and
// spoiled when `badCrc`).
struct PageSpec {
  uint32_t serial = 0;
  uint32_t sequence = 0;
  int64_t granule = -1;
  uint8_t flags = 0;
  Bytes lacing;
  Bytes body;
  bool badCrc = false;
  uint8_t version = 0;
};

inline Bytes page(const PageSpec& s) {
  Bytes b;
  b.insert(b.end(), {'O', 'g', 'g', 'S'});
  b.push_back(s.version);
  b.push_back(s.flags);
  putLe64(b, static_cast<uint64_t>(s.granule));
  putLe32(b, s.serial);
  putLe32(b, s.sequence);
  putLe32(b, 0);  // the CRC, filled in below
  b.push_back(static_cast<uint8_t>(s.lacing.size()));
  b.insert(b.end(), s.lacing.begin(), s.lacing.end());
  b.insert(b.end(), s.body.begin(), s.body.end());
  uint32_t crc = ogg::crc32(b.data(), b.size());
  if (s.badCrc) crc ^= 0x5A5A5A5Au;
  for (int i = 0; i < 4; ++i) b[22 + i] = static_cast<uint8_t>(crc >> (8 * i));
  return b;
}

// A packet's lacing values: 255s and a last one under 255.
inline Bytes lacingOf(size_t bytes) {
  Bytes l;
  while (bytes >= 255) {
    l.push_back(255);
    bytes -= 255;
  }
  l.push_back(static_cast<uint8_t>(bytes));
  return l;
}

class Muxer {
public:
  explicit Muxer(uint32_t serial, uint32_t maxSegments = 255) : serial_(serial), maxSegments_(maxSegments) {}

  Bytes out;
  std::vector<uint32_t> pageOffsets;  // where each page written starts

  uint32_t serial() const { return serial_; }
  uint32_t sequence() const { return seq_; }
  // The next page written continues... (for the odd cases).
  void setSequence(uint32_t seq) { seq_ = seq; }

  // A packet, which completes with the page's granule at `granule`.
  // Pages fill up and go out as its segments are added; the page it
  // completes on stays open for more packets until flush().
  void packet(const Bytes& p, int64_t granule) {
    const Bytes l = lacingOf(p.size());
    size_t at = 0;
    for (size_t i = 0; i < l.size(); ++i) {
      if (lacing_.size() >= maxSegments_) writePage(false);
      lacing_.push_back(l[i]);
      body_.insert(body_.end(), p.begin() + static_cast<long>(at), p.begin() + static_cast<long>(at + l[i]));
      at += l[i];
    }
    // It completes on the page being built: the page's granule is its.
    granule_ = granule;
    completes_ = true;
  }
  // Ends the page being built (if it has anything), the EOS flag on it.
  // An empty EOS page carries the stream's last granule.
  void flush(bool eos = false) {
    if (lacing_.empty() && eos) {
      granule_ = last_;
      completes_ = last_ >= 0;
    }
    if (!lacing_.empty() || eos) writePage(eos);
  }
  // A page with no segments (its granule -1, as the format has it, unless
  // a test says otherwise).
  void emptyPage(int64_t granule = -1) {
    flush(false);
    granule_ = granule;
    completes_ = granule >= 0;
    writePage(false);
  }
  // Something that isn't a page (junk between pages, a damaged stretch).
  void raw(const Bytes& b) {
    flush(false);
    out.insert(out.end(), b.begin(), b.end());
  }
  bool pending() const { return !lacing_.empty(); }

private:
  void writePage(bool eos) {
    PageSpec s;
    s.serial = serial_;
    s.sequence = seq_++;
    s.flags = static_cast<uint8_t>((first_ ? ogg::kBos : 0) | (continued_ ? ogg::kContinued : 0) | (eos ? ogg::kEos : 0));
    // The page's granule: the last packet completing on it; -1 when none does.
    s.granule = completes_ ? granule_ : -1;
    if (completes_) last_ = granule_;
    s.lacing = lacing_;
    s.body = body_;
    pageOffsets.push_back(static_cast<uint32_t>(out.size()));
    const Bytes b = page(s);
    out.insert(out.end(), b.begin(), b.end());
    continued_ = !lacing_.empty() && lacing_.back() == 255;
    lacing_.clear();
    body_.clear();
    first_ = false;
    completes_ = false;
  }

  uint32_t serial_;
  uint32_t maxSegments_;
  uint32_t seq_ = 0;
  bool first_ = true;
  bool continued_ = false;
  bool completes_ = false;  // a packet completed on the page being built
  int64_t granule_ = -1;
  int64_t last_ = -1;       // the last granule written
  Bytes lacing_;
  Bytes body_;
};

// ---- Opus packets ----

struct HeadSpec {
  uint8_t version = 1;
  uint8_t channels = 2;
  uint16_t preSkip = 312;
  uint32_t inputRate = 48000;
  int16_t gain = 0;
  uint8_t family = 0;
  uint8_t streams = 1;     // family 1 only
  uint8_t coupled = 1;
  Bytes map = {0, 1};      // family 1 only: channels entries
  size_t truncate = 0;     // cut the packet to this many bytes (0: whole)
};

inline Bytes opusHead(const HeadSpec& h) {
  Bytes b = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
  b.push_back(h.version);
  b.push_back(h.channels);
  putLe16(b, h.preSkip);
  putLe32(b, h.inputRate);
  putLe16(b, static_cast<uint16_t>(h.gain));
  b.push_back(h.family);
  if (h.family != 0) {
    b.push_back(h.streams);
    b.push_back(h.coupled);
    b.insert(b.end(), h.map.begin(), h.map.end());
  }
  if (h.truncate > 0 && h.truncate < b.size()) b.resize(h.truncate);
  return b;
}

// A comment header: the vendor string, and one comment padded out to make
// the packet `padTo` bytes (a picture's worth).
inline Bytes opusTags(size_t padTo = 0, const char* vendor = "oggwriter") {
  Bytes b = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
  const size_t vl = std::strlen(vendor);
  putLe32(b, static_cast<uint32_t>(vl));
  b.insert(b.end(), vendor, vendor + vl);
  if (padTo > b.size() + 8 + 2) {
    const size_t comment = padTo - b.size() - 8;
    putLe32(b, 1);
    putLe32(b, static_cast<uint32_t>(comment));
    b.push_back('X');
    b.push_back('=');
    b.resize(b.size() + comment - 2, 'p');
  } else {
    putLe32(b, 0);
  }
  return b;
}

// A frame size as a code-2 or VBR code-3 packet encodes it.
inline void putSize(Bytes& b, uint32_t size) {
  if (size < 252) {
    b.push_back(static_cast<uint8_t>(size));
  } else {
    b.push_back(static_cast<uint8_t>(252 + (size & 3)));
    b.push_back(static_cast<uint8_t>((size - 252 - (size & 3)) >> 2));
  }
}

// An Opus packet with TOC `config` (0-31) and the stereo bit, framed
// with `code` (RFC 6716 section 3.2): `sizes` the frames' byte counts
// (code 1: equal; code 3: `vbr` or equal, `padding` bytes of padding).
// The frames' bytes come from a seed (any value: nothing checks them).
inline Bytes opusPacket(uint8_t config, bool stereo, uint8_t code, const std::vector<uint16_t>& sizes, bool vbr = true,
                        uint32_t padding = 0, uint32_t seed = 1) {
  Bytes b;
  b.push_back(static_cast<uint8_t>((config << 3) | (stereo ? 4 : 0) | (code & 3)));
  if (code == 2) putSize(b, sizes[0]);
  if (code == 3) {
    b.push_back(static_cast<uint8_t>(sizes.size() | (vbr ? 0x80 : 0) | (padding ? 0x40 : 0)));
    if (padding) {
      uint32_t p = padding;
      while (p >= 255) {
        b.push_back(255);
        p -= 254;
      }
      b.push_back(static_cast<uint8_t>(p));
    }
    if (vbr) {
      for (size_t i = 0; i + 1 < sizes.size(); ++i) putSize(b, sizes[i]);
    }
  }
  uint32_t r = seed * 2654435761u + 12345u;
  for (const uint16_t s : sizes) {
    for (uint32_t i = 0; i < s; ++i) {
      r = r * 1103515245u + 12345u;
      b.push_back(static_cast<uint8_t>(r >> 16));
    }
  }
  for (uint32_t i = 0; i < padding; ++i) b.push_back(0);
  return b;
}

// The common case: one 20 ms CELT fullband frame (config 31, stereo: TOC 0xFC).
inline Bytes celt20ms(uint16_t bytes = 300, uint32_t seed = 1) { return opusPacket(31, true, 0, {bytes}, true, 0, seed); }

// ---- a whole file ----

struct FileSpec {
  HeadSpec head;
  uint32_t serial = 0x1234;
  std::vector<Bytes> packets;    // the audio packets, in order
  uint32_t packetsPerPage = 50;  // flush after this many (0: never: by maxSegments only)
  uint32_t maxSegments = 255;
  int64_t g0 = 0;                // the first page's granule above its samples (a cropped start; negative: invalid)
  int64_t endTrim = 0;           // samples trimmed off the end by the EOS page's granule
  bool eos = true;               // the last page flagged EOS
  size_t tagsPad = 0;            // the OpusTags packet's size (0: the minimum)
  Bytes trailing;                // bytes after the last page
};

struct Built {
  Bytes bytes;
  std::vector<uint32_t> pageOffsets;  // every page: the head's, the tags', the audio pages'
  uint32_t firstAudioPage = 0;
  int64_t lastGranule = -1;
  uint64_t samples = 0;  // the packets' samples, by their TOCs
  std::vector<int32_t> packetSamples;
  uint64_t kept = 0;  // lastGranule - g0 - preSkip: what the trimmed track holds
};

inline Built build(const FileSpec& f) {
  Built r;
  Muxer m(f.serial, f.maxSegments);
  m.packet(opusHead(f.head), 0);
  m.flush();
  m.packet(opusTags(f.tagsPad), 0);
  m.flush();
  r.firstAudioPage = static_cast<uint32_t>(m.out.size());
  int64_t k = f.g0;
  uint32_t onPage = 0;
  for (size_t i = 0; i < f.packets.size(); ++i) {
    const Bytes& p = f.packets[i];
    const int32_t s = oggopus::packetSamples(p.data(), p.size());
    r.packetSamples.push_back(s);
    if (s > 0) {
      k += s;
      r.samples += static_cast<uint64_t>(s);
    }
    const bool last = i + 1 == f.packets.size();
    m.packet(p, last ? k - f.endTrim : k);
    if (++onPage == f.packetsPerPage && f.packetsPerPage > 0 && !last) {
      m.flush();
      onPage = 0;
    }
  }
  m.flush(f.eos);
  r.lastGranule = k - f.endTrim;
  const int64_t kept = r.lastGranule - f.g0 - f.head.preSkip;
  r.kept = kept > 0 ? static_cast<uint64_t>(kept) : 0;
  r.bytes = m.out;
  r.bytes.insert(r.bytes.end(), f.trailing.begin(), f.trailing.end());
  r.pageOffsets = m.pageOffsets;
  return r;
}

// `n` packets of one 20 ms CELT frame each.
inline std::vector<Bytes> celtPackets(size_t n, uint16_t bytes = 300) {
  std::vector<Bytes> v;
  for (size_t i = 0; i < n; ++i) v.push_back(celt20ms(bytes, static_cast<uint32_t>(i + 1)));
  return v;
}

// trackseek::FileReader over the bytes (or their first `size`), counting.
class Reader : public trackseek::FileReader {
public:
  explicit Reader(const Bytes& b, uint32_t size = 0xFFFFFFFFu) : b_(b), size_(size < b.size() ? size : static_cast<uint32_t>(b.size())) {}
  uint32_t readAt(uint32_t offset, uint8_t* buf, uint32_t n) override {
    ++reads;
    if (offset >= size_) return 0;
    const uint32_t got = size_ - offset < n ? size_ - offset : n;
    std::memcpy(buf, b_.data() + offset, got);
    bytes += got;
    return got;
  }
  uint32_t size() const { return size_; }
  uint32_t reads = 0;
  uint64_t bytes = 0;

private:
  const Bytes& b_;
  uint32_t size_;
};

}  // namespace oggwriter
