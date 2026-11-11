// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TagText.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "QueueView.h"

namespace tagtext {

using cardcontract::RunFields;
namespace mptg = cardcontract::mptg;

const char* const kHelp =
    "g report (where the names came from), g0 walk /music and build again, g<n> a synthetic library of n tracks "
    "(1-50000), gs the scan's status (gs0 its figures from now), gt</music/...> one file's tags (read now, D, T, the "
    "winner), gr Rescan tags (gr! the transfer's files too), gw walk now, gb build now (gb! deferred to the next "
    "boot), gv verify the transfer's files; gc the sector cache (gc0 off, gc1 on, gc2 on and every hit checked), gl "
    "the card's bench (glw with the walks); gw? the waveform index's state (gw=<99 digits 0-9> a shape with no card, "
    "gw#<480 hex> 240 bars through the reduction, gw- drops it); gm the mood words (gm<hex> the tracks and albums "
    "filter <hex> lets through, gm127 Sad, Relaxed, Vocals, Danceable); "
    "gx the card's write bench (gx<n> n MB a run, 1-64; gxs the clock soak at "
    "20 and 26.67 MHz, gxs<n> n MB, 1-256; gxc the CRC16s timed, gxc0 the driver on the stock one, gxc1 on the fast)";

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int hexOf(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

size_t clampLen(int n, size_t size) {
  if (size == 0 || n < 0) return 0;
  return static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1;
}

// A line, appended to a piece at a time (never past its buffer: a list of
// 2.3.6's 1,023 bytes and its quotes fit), then handed out.
struct Line {
  char buf[1200];
  size_t len = 0;
  Line() { buf[0] = 0; }
  __attribute__((format(printf, 2, 3))) void add(const char* fmt, ...) {
    if (len + 1 >= sizeof(buf)) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
    va_end(ap);
    len += clampLen(n, sizeof(buf) - len);
  }
  void out(LineFn fn, void* ctx) {
    if (len) fn(ctx, buf);
    len = 0;
    buf[0] = 0;
  }
};

// A list field's values (U+001F between them) as "a" | "b".
void addList(Line& l, const char* s, size_t n) {
  size_t start = 0;
  bool first = true;
  for (size_t i = 0; i <= n; ++i) {
    if (i < n && s[i] != '\x1F') continue;
    l.add("%s\"%.*s\"", first ? "" : " | ", static_cast<int>(i - start), s + start);
    first = false;
    start = i + 1;
  }
}

// A gain in hundredths of a dB: "-6.50 dB".
void addGain(Line& l, int16_t g) {
  const int v = g < 0 ? -static_cast<int>(g) : g;
  l.add("%s%d.%02d dB", g < 0 ? "-" : "+", v / 100, v % 100);
}

// A peak in ten-thousandths: "0.9876".
void addPeak(Line& l, uint16_t p) { l.add("%u.%04u", static_cast<unsigned>(p / 10000), static_cast<unsigned>(p % 10000)); }

const char* mimeName(uint8_t m) {
  switch (m) {
    case mptg::kMimeJpeg: return "JPEG";
    case mptg::kMimePng: return "PNG";
    case mptg::kMimeOther: return "another image type";
    default: return "no image type";
  }
}

const char* codingName(uint8_t c) {
  switch (c) {
    case mptg::kCodingRaw: return "raw";
    case mptg::kCodingUnsync: return "ID3 unsynchronised";
    case mptg::kCodingOggBase64: return "base64 across Ogg pages";
    case mptg::kCodingApe: return "an APE item";
    default: return "?";
  }
}

const char* containerName(uint8_t c) {
  switch (c) {
    case mptg::kContainerMp3: return "MP3";
    case mptg::kContainerFlac: return "FLAC";
    case mptg::kContainerOpus: return "Opus";
    case mptg::kContainerNotAudio: return "not audio";
    default: return "unknown";
  }
}

}  // namespace

Parsed parse(const char* arg) {
  Parsed p;
  if (!arg) arg = "";
  while (isSpace(*arg)) ++arg;
  size_t n = std::strlen(arg);
  while (n && isSpace(arg[n - 1])) --n;
  if (n == 0) {
    p.command = Command::Report;
    return p;
  }
  if (arg[0] >= '0' && arg[0] <= '9') {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
      if (arg[i] < '0' || arg[i] > '9') return p;  // Bad
      if (v <= 100000) v = v * 10 + static_cast<uint32_t>(arg[i] - '0');
    }
    if (v == 0) {
      p.command = Command::Rebuild;
    } else if (v <= 50000) {
      p.command = Command::Synthetic;
      p.n = static_cast<uint32_t>(v);
    }
    return p;
  }
  if (arg[0] == 't') {
    const char* path = arg + 1;
    while (isSpace(*path)) ++path;
    if (*path) {
      p.command = Command::Dump;
      p.path = path;  // (the caller's string: its trailing spaces are the caller's to trim)
    }
    return p;
  }
  if (arg[0] == 'm') {
    // gm alone, or a filter's q in 1-4 hex digits of either case; one that
    // isn't a filter (docs/MOOD-FILTERS.md 5.4) is Bad, as g<n> past 50,000.
    if (n == 1) {
      p.command = Command::Moods;
      p.n = kMoodsReport;
      return p;
    }
    if (n > 5) return p;
    uint32_t q = 0;
    for (size_t i = 1; i < n; ++i) {
      const int h = hexOf(arg[i]);
      if (h < 0) return p;
      q = q << 4 | static_cast<uint32_t>(h);
    }
    if (moodfilter::valid(static_cast<uint16_t>(q))) {
      p.command = Command::Moods;
      p.n = q;
    }
    return p;
  }
  if (arg[0] == 'x') {
    if (n >= 2 && arg[1] == 'c') {
      // gxc: the CRC16s timed; gxc0, gxc1: the driver's switched.
      if (n == 2) {
        p.command = Command::CrcBench;
        p.n = kCrcReport;
      } else if (n == 3 && (arg[2] == '0' || arg[2] == '1')) {
        p.command = Command::CrcBench;
        p.n = static_cast<uint32_t>(arg[2] - '0');
      }
      return p;
    }
    // gx, gx<n>; gxs, gxs<n>: a size of 1 to the command's most, in MB.
    size_t i = 1;
    Command c = Command::WriteBench;
    uint32_t v = kWriteBenchMb, most = kWriteBenchMaxMb;
    if (i < n && arg[i] == 's') {
      c = Command::WriteSoak;
      v = kWriteSoakMb;
      most = kWriteSoakMaxMb;
      ++i;
    }
    if (i < n) {
      v = 0;
      for (; i < n; ++i) {
        if (arg[i] < '0' || arg[i] > '9') return p;  // Bad
        v = v * 10 + static_cast<uint32_t>(arg[i] - '0');
        if (v > most) return p;  // Bad (and never past a uint32_t)
      }
      if (v == 0) return p;
    }
    p.command = c;
    p.n = v;
    return p;
  }
  if (n == 1) {
    switch (arg[0]) {
      case 's': p.command = Command::Status; break;
      case 'r': p.command = Command::Rescan; break;
      case 'w': p.command = Command::Walk; break;
      case 'b': p.command = Command::Build; break;
      case 'v': p.command = Command::Verify; break;
      case 'c':
        p.command = Command::Cache;
        p.n = kCacheReport;
        break;
      case 'l': p.command = Command::Bench; break;
      default: break;
    }
    return p;
  }
  if (arg[0] == 'w') {
    // (gw alone is the walk, above.) What gw= and gw# hold is digitsIn()'s and
    // hexIn()'s to judge.
    const bool alone = arg[1] == '?' || arg[1] == '-';
    if ((alone && n == 2) || arg[1] == '=' || arg[1] == '#') {
      p.command = Command::Wave;
      p.path = arg + 1;
    }
    return p;
  }
  if (n == 2 && arg[0] == 'r' && arg[1] == '!') p.command = Command::RescanAll;
  if (n == 2 && arg[0] == 's' && arg[1] == '0') {
    p.command = Command::Status;
    p.n = 1;
  }
  if (n == 2 && arg[0] == 'b' && arg[1] == '!') {
    p.command = Command::Build;
    p.n = 1;
  }
  if (n == 2 && arg[0] == 'c' && arg[1] >= '0' && arg[1] <= '2') {
    p.command = Command::Cache;
    p.n = static_cast<uint32_t>(arg[1] - '0');
  }
  if (n == 2 && arg[0] == 'l' && arg[1] == 'w') {
    p.command = Command::Bench;
    p.n = 1;
  }
  return p;
}

bool digitsIn(const char* text, uint8_t* out, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;  // (the NUL of one too short too)
    out[i] = static_cast<uint8_t>(text[i] - '0');
  }
  return text[count] == 0;
}

bool hexIn(const char* text, uint8_t* out, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const int hi = hexOf(text[2 * i]);
    const int lo = hi < 0 ? -1 : hexOf(text[2 * i + 1]);  // (no read past the NUL of one too short)
    if (lo < 0) return false;
    out[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  return text[2 * count] == 0;
}

const char* statusName(LibraryBuilder::Status s) {
  switch (s) {
    case LibraryBuilder::Status::Scanned: return "Scanned";
    case LibraryBuilder::Status::Software: return "Software";
    case LibraryBuilder::Status::Pending: return "Pending";
    case LibraryBuilder::Status::Unreadable: return "Unreadable";
  }
  return "?";
}

const char* pickName(LibraryBuilder::Pick p) {
  switch (p) {
    case LibraryBuilder::Pick::Transfer: return "the transfer's record";
    case LibraryBuilder::Pick::Device: return "the device's record";
    case LibraryBuilder::Pick::Path: return "the path";
  }
  return "?";
}

size_t fatTimeText(uint32_t fatTime, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  if (fatTime == 0) return clampLen(snprintf(buf, size, "none"), size);
  const uint32_t date = fatTime >> 16, time = fatTime & 0xFFFF;
  return clampLen(snprintf(buf, size, "%04u-%02u-%02u %02u:%02u:%02u", static_cast<unsigned>(1980 + (date >> 9)),
                           static_cast<unsigned>((date >> 5) & 15), static_cast<unsigned>(date & 31),
                           static_cast<unsigned>(time >> 11), static_cast<unsigned>((time >> 5) & 63),
                           static_cast<unsigned>((time & 31) * 2)),
                  size);
}

size_t lengthText(uint32_t ms, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  if (ms == 0) return clampLen(snprintf(buf, size, "unknown"), size);
  const uint32_t s = ms / 1000;
  return clampLen(snprintf(buf, size, "%lu:%02lu.%03lu", static_cast<unsigned long>(s / 60),
                           static_cast<unsigned long>(s % 60), static_cast<unsigned long>(ms % 1000)),
                  size);
}

const char* camelotName(uint8_t c) {
  static const char* const kNames[24] = {"1A", "2A", "3A", "4A", "5A", "6A", "7A", "8A", "9A", "10A", "11A", "12A",
                                         "1B", "2B", "3B", "4B", "5B", "6B", "7B", "8B", "9B", "10B", "11B", "12B"};
  return c >= 1 && c <= 24 ? kNames[c - 1] : "";
}

size_t flagsText(uint16_t flags, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  buf[0] = 0;
  size_t at = 0;
  auto add = [&](uint16_t bit, const char* name) {
    if (!(flags & bit) || at + 1 >= size) return;
    at += clampLen(snprintf(buf + at, size - at, "%s%s", at ? ", " : "", name), size - at);
  };
  add(mptg::kNoTags, "NO_TAGS");
  add(mptg::kUnreadable, "UNREADABLE");
  add(mptg::kTruncated, "TRUNCATED");
  add(mptg::kBpmAnalysed, "BPM_ANALYSED");
  add(mptg::kFromApi, "FROM_API");
  add(mptg::kRgFromR128, "RG_FROM_R128");
  return at;
}

void dumpRecord(const mptg::Record& r, const RunFields* run, LineFn out, void* ctx) {
  if (!out) return;
  Line l;
  char t[64];
  if (r.flags & mptg::kUnreadable) {
    l.add("  UNREADABLE: the producer couldn't parse it (no field)");
    l.out(out, ctx);
    return;
  }
  // The text fields, in the run's order.
  static const char* const kNames[cardcontract::kRunFields] = {
      "title", "artist", "album", "album artist", "genre", "composer", "title sort", "artist sort", "album sort",
      "album artist sort", "MusicBrainz album", "MusicBrainz recording"};
  bool anyText = false;
  for (uint32_t f = 0; run && f < cardcontract::kRunFields; ++f) {
    if (!run->has(f)) continue;
    anyText = true;
    l.add("  %s: ", kNames[f]);
    if (cardcontract::isListField(f)) {
      addList(l, run->get(f), run->len(f));
    } else {
      l.add("\"%.*s\"", static_cast<int>(run->len(f)), run->get(f));
    }
    l.out(out, ctx);
  }
  if (!anyText) {
    l.add("  no text fields");
    l.out(out, ctx);
  }
  // The numbers.
  l.add("  %s", containerName(r.container));
  if (r.year) l.add(", year %u", static_cast<unsigned>(r.year));
  if (r.track) {
    l.add(", track %u", static_cast<unsigned>(r.track));
    if (r.trackTotal) l.add("/%u", static_cast<unsigned>(r.trackTotal));
  }
  if (r.disc) {
    l.add(", disc %u", static_cast<unsigned>(r.disc));
    if (r.discTotal) l.add("/%u", static_cast<unsigned>(r.discTotal));
  }
  lengthText(r.durationMs, t, sizeof(t));
  l.add(", length %s", t);
  if (r.bpm10) l.add(", BPM %u.%u", static_cast<unsigned>(r.bpm10 / 10), static_cast<unsigned>(r.bpm10 % 10));
  const char* key = camelotName(mptg::camelotOf(r));
  if (key[0]) l.add(", key %s", key);
  const uint8_t comp = mptg::compilationOf(r);
  if (comp == 1) l.add(", a compilation");
  if (comp == 2) l.add(", not a compilation");
  l.out(out, ctx);
  if (r.flags & (mptg::kHasRgTrack | mptg::kHasRgAlbum)) {
    l.add("  ReplayGain:");
    if (r.flags & mptg::kHasRgTrack) {
      l.add(" track ");
      addGain(l, r.rgTrackGain);
      if (r.rgTrackPeak) {
        l.add(" peak ");
        addPeak(l, r.rgTrackPeak);
      }
    }
    if (r.flags & mptg::kHasRgAlbum) {
      l.add("%s album ", (r.flags & mptg::kHasRgTrack) ? "," : "");
      addGain(l, r.rgAlbumGain);
      if (r.rgAlbumPeak) {
        l.add(" peak ");
        addPeak(l, r.rgAlbumPeak);
      }
    }
    l.out(out, ctx);
  }
  if (mptg::hasPicture(r)) {
    char a[16], b[16];
    queueview::grouped(r.picLength, a, sizeof(a));
    queueview::grouped(r.picOffset, b, sizeof(b));
    l.add("  picture: %s, type %u%s, %s B at %s (%s)", mimeName(mptg::picMimeOf(r)), static_cast<unsigned>(r.picType),
          r.picType == 3 ? " (front cover)" : "", a, b, codingName(r.picCoding));
    l.out(out, ctx);
  }
  flagsText(r.flags, t, sizeof(t));
  if (t[0]) {
    l.add("  flags: %s", t);
    l.out(out, ctx);
  }
  if (r.known != mptg::kKnownRules1) {
    l.add("  looked for: 0x%05lx (rules 1 look for all of 0x%05lx)", static_cast<unsigned long>(r.known),
          static_cast<unsigned long>(mptg::kKnownRules1));
    l.out(out, ctx);
  }
}

Sources countSources(const LibraryIndex& index) {
  Sources s;
  if (!index.ready()) return s;
  s.tracks = index.trackCount();
  for (uint32_t i = 0; i < s.tracks; ++i) {
    const uint8_t f = index.track(i).flags;
    switch (f & LibraryIndex::kSourceMask) {
      case LibraryIndex::kFromTransfer: ++s.transfer; break;
      case LibraryIndex::kFromDevice: ++s.device; break;
      default: ++s.path; break;
    }
    if (f & LibraryIndex::kTrackPending) ++s.pending;
  }
  return s;
}

size_t sourcesText(const Sources& s, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  buf[0] = 0;
  char n[16];
  size_t at = clampLen(snprintf(buf, size, "%s track%s", queueview::grouped(s.tracks, n, sizeof(n)),
                                s.tracks == 1 ? "" : "s"),
                       size);
  if (s.tracks == 0) return at;
  const char* sep = ": ";
  auto part = [&](uint32_t count, const char* what) {
    if (!count || at + 1 >= size) return;
    at += clampLen(snprintf(buf + at, size - at, "%s%s %s", sep, queueview::grouped(count, n, sizeof(n)), what),
                   size - at);
    sep = ", ";
  };
  part(s.transfer, "from the transfer's records");
  part(s.device, "from the device's");
  part(s.path, s.transfer || s.device ? "by their paths" : "named by their paths");
  if (s.pending && at + 1 < size) {
    at += clampLen(snprintf(buf + at, size - at, " (%s for the scan)", queueview::grouped(s.pending, n, sizeof(n))),
                   size - at);
  }
  return at;
}

// ---- moods ----

Moods countMoods(const LibraryIndex& index) {
  Moods m;
  if (!index.ready()) return m;
  m.tracks = index.trackCount();
  m.withWord = index.moodTracks();
  m.blockBytes = index.memory().moods;
  return m;
}

size_t moodsText(const Moods& m, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  char a[16], b[16], c[16];
  queueview::grouped(m.tracks, b, sizeof(b));
  if (m.blockBytes == 0) {
    return clampLen(snprintf(buf, size, "no mood data: none of %s track%s has a word, block 0 B", b,
                             m.tracks == 1 ? "" : "s"),
                    size);
  }
  return clampLen(snprintf(buf, size, "%s of %s tracks with a word (%.1f %%), block %s B",
                           queueview::grouped(m.withWord, a, sizeof(a)), b,
                           100.0 * m.withWord / (m.tracks ? m.tracks : 1),
                           queueview::grouped(static_cast<uint32_t>(m.blockBytes), c, sizeof(c))),
                  size);
}

size_t filterText(uint16_t q, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  using moodfilter::Scale;
  const moodfilter::Filter f = moodfilter::Filter::unpack(q);
  static const char* const kValence[4] = {"", "happy", "neutral", "sad"};
  static const char* const kEnergy[4] = {"", "relaxed", "neutral", "aggressive"};
  static const char* const kVoice[3] = {"", "instrumental", "vocals"};
  static const char* const kTags[3] = {"acoustic", "party", "danceable"};
  // name, need, under: moodfilter::Level's order (5.4).
  static const char* const kLevels[3] = {"Standard", "Strict", "Loose"};
  static const uint8_t kNeed[3] = {2, 3, 1}, kUnder[3] = {2, 1, 3};
  buf[0] = 0;
  size_t at = 0;
  const char* sep = "";
  auto add = [&](const char* fmt, const char* a, const char* b) {
    if (at + 1 >= size) return;
    at += clampLen(snprintf(buf + at, size - at, fmt, a, b), size - at);
  };
  if (f.valence != Scale::Any) {
    add("%svalence %s", sep, kValence[static_cast<int>(f.valence)]);
    sep = "; ";
  }
  if (f.energy != Scale::Any) {
    add("%senergy %s", sep, kEnergy[static_cast<int>(f.energy)]);
    sep = "; ";
  }
  if (f.voice != moodfilter::Voice::Any) {
    add("%svoice %s", sep, kVoice[static_cast<int>(f.voice)]);
    sep = "; ";
  }
  if (f.tags) {
    add("%s%s", sep, "tags ");
    const char* comma = "";
    for (int t = 0; t < 3; ++t) {
      if (!(f.tags & (1u << t))) continue;
      add("%s%s", comma, kTags[t]);
      comma = ", ";
    }
  }
  if (!at) add("%s%s", "no group", "");
  const int l = static_cast<int>(f.level);
  if (at + 1 < size) {
    at += clampLen(snprintf(buf + at, size - at, " at %s (need %u, under %u)%s", kLevels[l],
                            static_cast<unsigned>(kNeed[l]), static_cast<unsigned>(kUnder[l]),
                            f.active() ? "" : ": filters nothing"),
                   size - at);
  }
  return at;
}

TrackFits fittingTracks(const LibraryIndex& index, const moodfilter::Filter& f) {
  TrackFits out;
  if (!index.ready()) return out;
  out.tracks = index.trackCount();
  if (const uint16_t* words = index.moodWords()) {
    out.fit = moodfilter::count(words, out.tracks, f, &out.analysed);
  } else {
    out.fit = f.active() ? 0 : out.tracks;  // every word 0
  }
  return out;
}

bool albumFits(const LibraryIndex& index, uint32_t album, const moodfilter::Filter& f, moodfilter::Tally* t) {
  moodfilter::Tally got;
  if (const uint16_t* words = index.moodWords()) {
    const LibraryIndex::Span s = index.tracksOfAlbum(album);
    got = moodfilter::tally(words, s.ids, s.count, f);
  } else {
    got.unfiltered = !f.active();  // every word 0: no analysed song
  }
  if (t) *t = got;
  return moodfilter::majority(got);
}

AlbumFits fittingAlbums(const LibraryIndex& index, const moodfilter::Filter& f) {
  AlbumFits out;
  if (!index.ready()) return out;
  const LibraryIndex::Span az = index.albumsAZ();
  out.albums = az.count;
  for (uint32_t i = 0; i < az.count; ++i) {
    moodfilter::Tally t;
    out.fit += albumFits(index, az[i], f, &t);
    out.analysed += t.analysed > 0;
  }
  return out;
}

size_t tailText(const uint8_t* tail, bool taken, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  if (!tail) return clampLen(snprintf(buf, size, "none (a stride under 80 carries none)"), size);
  size_t at = 0;
  bool zeros = true;
  for (uint32_t i = 0; i < 8; ++i) {
    at += clampLen(snprintf(buf + at, size - at, "%s%02X", i ? " " : "", static_cast<unsigned>(tail[i])), size - at);
    zeros = zeros && tail[i] == 0;
  }
  if (at + 1 >= size) return at;
  if (taken) {
    at += clampLen(snprintf(buf + at, size - at, ", taken: "), size - at);
    return at + (at + 1 < size ? wordText(moodfilter::wordOfTail(tail), buf + at, size - at) : 0);
  }
  return at + clampLen(snprintf(buf + at, size - at, "%s",
                                zeros ? ": no server row (word 0)"
                                      : ": REFUSED (no PRESENT bit, flag bits 3-7 or byte 79 set, or a value over "
                                        "100 not 255): no word, the record is fine"),
                       size - at);
}

size_t wordText(uint16_t word, char* buf, size_t size) {
  if (!buf || size == 0) return 0;
  size_t at = clampLen(snprintf(buf, size, "0x%04X", static_cast<unsigned>(word)), size);
  if (word == 0) return at + clampLen(snprintf(buf + at, size - at, ": no row"), size - at);
  if (!(word & moodfilter::kAnalysed)) return at + clampLen(snprintf(buf + at, size - at, ": not analysed"), size - at);
  const char* sep = ": ";
  // A field: its name, the side a scale's bucket leans to (none at bucket
  // 0), the bucket; or NULL.
  auto field = [&](const char* name, const char* side, bool null, uint32_t bucket) {
    if (at + 1 >= size) return;
    if (null) {
      at += clampLen(snprintf(buf + at, size - at, "%s%s NULL", sep, name), size - at);
    } else {
      const bool leans = bucket && side[0];
      at += clampLen(snprintf(buf + at, size - at, "%s%s%s%s %u", sep, name, leans ? " " : "", leans ? side : "",
                              static_cast<unsigned>(bucket)),
                     size - at);
    }
    sep = ", ";
  };
  // A scale's field is side<<2 | bucket, 4 NULL (5.4).
  const uint32_t valence = word >> 11 & 7, energy = word >> 8 & 7;
  field("valence", valence & 4 ? "sad" : "happy", valence == 4, valence & 3);
  field("energy", energy & 4 ? "aggressive" : "relaxed", energy == 4, energy & 3);
  field("instrumental", "", (word & moodfilter::kVoiceNull) != 0, word >> 6 & 3);
  field("acoustic", "", false, word >> 4 & 3);
  field("party", "", false, word >> 2 & 3);
  field("danceable", "", false, word & 3);
  return at;
}

}  // namespace tagtext
