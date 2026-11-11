// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "StatsFile.h"

#include <cstdio>
#include <cstring>
#include <new>

namespace statsfile {

namespace cc = cardcontract;
namespace sf = statsfold;
using cc::Why;
using tagstore::File;
using tagstore::Fs;

namespace {

const cc::SectionSpec kStatsSecs[sf::kSecs] = {
    {sf::kTypes[sf::kHead], sf::kStrides[sf::kHead], true}, {sf::kTypes[sf::kMark], sf::kStrides[sf::kMark], true},
    {sf::kTypes[sf::kHour], sf::kStrides[sf::kHour], true}, {sf::kTypes[sf::kDays], sf::kStrides[sf::kDays], true},
    {sf::kTypes[sf::kRcnt], sf::kStrides[sf::kRcnt], true}, {sf::kTypes[sf::kTrkd], sf::kStrides[sf::kTrkd], true}};
const cc::SectionSpec kBaseSecs[1] = {{sf::kTrks, sf::kTrackBytes, true}};

constexpr uint32_t kWaits = sizeof(tagstore::kBootWaitsMs) / sizeof(tagstore::kBootWaitsMs[0]);

// A file written through `buf` (kBufBytes) in pieces that end on the file's
// 4 KB boundaries (tagstore::writePiece()): a run of writes each starting
// where the last ended is gathered; a write anywhere else (the writer's gap
// zeros at its begin(), its directory and header at its finish()) ends the
// run first.
class PieceSink : public cc::Sink {
public:
  PieceSink(File& f, uint8_t* buf) : f_(f), buf_(buf) {}
  bool write(uint32_t offset, const void* data, uint32_t n) override {
    if (bad_) return false;
    if (n == 0) return true;
    if (len_ && offset != at_ + len_ && !flush()) return false;
    if (!len_) at_ = offset;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (n) {
      // From at_ to the next boundary is at most kBufBytes.
      const uint32_t k = tagstore::writePiece(at_ + len_, n);
      std::memcpy(buf_ + len_, p, k);
      len_ += k;
      p += k;
      n -= k;
      if ((at_ + len_) % tagstore::kWriteAlign == 0 && !flush()) return false;
    }
    return true;
  }
  bool flush() {
    if (len_ && !bad_ && !f_.write(at_, buf_, len_)) bad_ = true;
    at_ += len_;
    len_ = 0;
    return !bad_;
  }

private:
  File& f_;
  uint8_t* buf_;
  uint32_t at_ = 0;
  uint32_t len_ = 0;
  bool bad_ = false;
};

// A reader's Io on a file whose own records are damaged is the file's
// fault: no read again changes it (cardroot's rule).
Why ofFile(const File& f, Why w) { return w == Why::Io && f.damaged() ? Why::FatRecords : w; }

// One section of stats.bin into the Fold, its CRC taken as it streams: HEAD
// (its first 128 bytes), or an array's rows in batches of `b`, read by
// their prefix. A row out of order stops the load, not the stream: the CRC
// says whether the bytes or the rows are wrong.
Why loadSection(sf::Fold& fold, cc::Source& src, const cc::Section& s, sf::Sec sec, uint8_t* a, uint8_t* b,
                bool* noMemory) {
  cc::Stream st;
  st.begin(&src, s.offset, s.offset + s.bytes, a, kBufBytes);
  if (sec == sf::kHead) {
    if (!st.read(b, sf::kHeadBytes) || !st.drain()) return Why::Io;
    if (st.crc() != s.crc) return Why::SectionCrc;
    fold.loadHead(b);
    return Why::Ok;
  }
  const uint32_t per = kBufBytes / s.stride;
  if (per == 0) return Why::Shape;  // rows longer than 4 KB: no minor writes them
  bool order = true;
  for (uint32_t left = s.count; left;) {
    const uint32_t k = left < per ? left : per;
    if (!st.read(b, k * s.stride)) return Why::Io;
    if (order && !fold.loadRows(sec, b, k, s.stride)) {
      if (fold.noMemory()) {
        *noMemory = true;
        return Why::Ok;
      }
      order = false;
    }
    left -= k;
  }
  if (st.crc() != s.crc) return Why::SectionCrc;
  return order ? Why::Ok : Why::Order;
}

// A base's TRKS rows streamed whole, none kept: their CRC, then their order.
Why scanRows(cc::Source& src, const cc::Section& s, uint8_t* buf) {
  cc::Stream st;
  st.begin(&src, s.offset, s.offset + s.bytes, buf, kBufBytes);
  bool order = true;
  uint64_t last = 0;
  for (uint32_t i = 0; i < s.count; ++i) {
    uint8_t h[8];
    if (!st.read(h, sizeof(h)) || !st.read(nullptr, s.stride - sizeof(h))) return Why::Io;
    const uint64_t hash = cc::get64(h);
    if (i && hash <= last) order = false;
    last = hash;
  }
  if (st.crc() != s.crc) return Why::SectionCrc;
  return order ? Why::Ok : Why::Order;
}

bool same(const Store::Copy& a, const Store::Copy& b) {
  return a.present == b.present && (!a.present || (a.generation == b.generation && a.headerCrc == b.headerCrc));
}

}  // namespace

const cc::FormatSpec kStatsSpec{kMagic, kHeaderBytes, kStatsSecs, sf::kSecs, kMajor};
const cc::FormatSpec kBaseSpec{kMagic, kHeaderBytes, kBaseSecs, 1, kMajor};

bool baseName(uint32_t gen, char* out, size_t cap) {
  const int n = std::snprintf(out, cap, "/.player/stats-%08lx.bin", static_cast<unsigned long>(gen));
  return n > 0 && static_cast<size_t>(n) < cap;
}

void baseFrame(uint32_t gen, uint32_t rows, uint32_t rowsCrc, uint8_t out[kBaseRowsAt], uint32_t* headerCrc) {
  std::memset(out, 0, kBaseRowsAt);
  cc::put32(out, kMagic);
  cc::put16(out + 4, kMajor);
  cc::put16(out + 6, kMinor);
  cc::put32(out + 8, kHeaderBytes);
  cc::put32(out + 12, 1);
  cc::put32(out + 16, kBaseRowsAt + rows * sf::kTrackBytes);
  cc::put32(out + 20, gen);  // cardId 0 at 24; the header's CRC at 32, 0 while it is taken
  uint8_t* e = out + kHeaderBytes;
  cc::put32(e, sf::kTrks);
  cc::put32(e + 4, cc::kSectionRequired);
  cc::put32(e + 8, kBaseRowsAt);
  cc::put32(e + 12, rows * sf::kTrackBytes);
  cc::put32(e + 16, rows);
  cc::put32(e + 20, sf::kTrackBytes);
  cc::put32(e + 24, rowsCrc);
  const uint32_t crc = cc::crc32(out, kBaseRowsAt);
  cc::put32(out + 32, crc);
  if (headerCrc) *headerCrc = crc;
}

Why checkWhole(cc::Source& src, const cc::FormatSpec& spec, uint8_t* buf, uint32_t bytes) {
  cc::Container c;
  Why w = c.open(src, spec, nullptr);
  for (uint32_t i = 0; w == Why::Ok && i < spec.count; ++i)
    if (c.section(i).present()) w = c.checkCrc(i, buf, bytes);
  return w;
}

tagstore::TmpCheck::Verdict StatsCheck::check(Fs& fs, const char* tmp) {
  why_ = Why::Ok;
  if (!buf_ || bytes_ == 0) return Verdict::Unknown;  // no memory: no verdict
  bool cardFailed = false;
  File* f = fs.openRead(tmp, &cardFailed);
  if (!f) {
    why_ = cardFailed ? Why::Io : Why::Missing;
    return cardFailed ? Verdict::Unknown : Verdict::Torn;
  }
  tagstore::ReadWatch src(f);
  why_ = ofFile(*f, checkWhole(src, kStatsSpec, buf_, bytes_));
  fs.close(f);
  // A newer stats firmware's file is whole to this one: promoted, then read
  // as Newer and left alone (5.12). A read the card failed, or damaged
  // records, is no verdict: the tmp may be the only copy.
  if (why_ == Why::Ok || why_ == Why::Major || why_ == Why::UnknownRequired) return Verdict::Whole;
  if (why_ == Why::Io || why_ == Why::FatRecords) return Verdict::Unknown;
  return Verdict::Torn;
}

const char* readName(Read r) {
  switch (r) {
    case Read::Loaded: return "loaded";
    case Read::Absent: return "absent";
    case Read::Corrupt: return "corrupt";
    case Read::Newer: return "newer";
    case Read::Unread: return "unread";
    case Read::NoMemory: return "noMemory";
  }
  return "?";
}

const char* savedName(Saved s) {
  switch (s) {
    case Saved::Wrote: return "wrote";
    case Saved::Nothing: return "nothing new";
    case Saved::Off: return "off";
    case Saved::Changed: return "changed";
    case Saved::NoMemory: return "no memory";
    case Saved::Failed: return "failed";
    case Saved::Held: return "held";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// The read.
// ---------------------------------------------------------------------------
Store::Again Store::readStats(sf::Fold& fold, Scratch* sc, uint8_t* a, uint8_t* b, Opened* o) {
  fold.clear();
  o->bytes = o->generation = o->headerCrc = 0;
  bool cardFailed = false;
  File* f = fs_.openRead(kStatsBin, &cardFailed);
  if (!f) {
    // Absent only when the card was read and it isn't there; else (the card
    // failed, or the records of the way to it are damaged) nothing is known.
    if (!cardFailed && fs_.look(kStatsBin) == Fs::Seen::Gone) {
      o->read = Read::Absent;
      o->why = Why::Ok;
      return Again::Final;
    }
    o->read = Read::Unread;
    o->why = Why::Io;
    return Again::CardFault;
  }
  tagstore::ReadWatch src(f);
  o->bytes = src.size();
  cc::Container& c = sc->frame;
  Why w = c.open(src, kStatsSpec, nullptr);
  if (w == Why::Ok && c.section(sf::kHead).bytes < sf::kHeadBytes) w = Why::Shape;
  bool noMemory = false;
  if (w == Why::Ok) {
    if (!fold.reserveLoad(c.section(sf::kRcnt).count, c.section(sf::kTrkd).count)) {
      noMemory = true;
    } else {
      fold.loadGeneration(c.header().generation);
      for (uint32_t i = 0; i < sf::kSecs && w == Why::Ok && !noMemory; ++i)
        w = loadSection(fold, src, c.section(i), static_cast<sf::Sec>(i), a, b, &noMemory);
    }
  }
  w = ofFile(*f, w);
  fs_.close(f);
  if (noMemory) {
    fold.clear();
    o->read = Read::NoMemory;
    o->why = Why::Ok;
    return Again::Final;
  }
  o->why = w;
  if (w == Why::Ok) {
    o->read = Read::Loaded;
    o->generation = c.header().generation;
    o->headerCrc = c.header().headerCrc;
    return Again::Final;
  }
  fold.clear();
  if (w == Why::Major || w == Why::UnknownRequired) {
    o->read = Read::Newer;
    return Again::Final;
  }
  if (w == Why::Io || w == Why::FatRecords) {
    o->read = Read::Unread;
    return w == Why::Io ? Again::CardFault : Again::Final;
  }
  o->read = Read::Corrupt;
  return Again::Corrupt;
}

Store::Again Store::checkBase(const sf::Head& h, Scratch* sc, uint8_t* a, Opened* o, bool* lost) {
  *lost = false;
  o->baseBytes = 0;
  char name[kNameCap];
  baseName(h.baseGen, name, sizeof(name));
  bool cardFailed = false;
  File* f = fs_.openRead(name, &cardFailed);
  if (!f) {
    // Lost only when the card answers that it isn't there.
    if (!cardFailed && fs_.look(name) == Fs::Seen::Gone) {
      *lost = true;
      o->why = Why::Ok;
      return Again::Final;
    }
    o->why = Why::Io;
    return Again::CardFault;
  }
  tagstore::ReadWatch src(f);
  o->baseBytes = src.size();
  Why w = Why::Ok;
  uint8_t head[kHeaderBytes];
  if (src.size() < kHeaderBytes) {
    w = Why::Short;  // its header doesn't read whole: never a lost base
  } else if (!src.read(0, head, kHeaderBytes)) {
    w = Why::Io;
  } else if (cc::get32(head + 32) != h.baseCrc) {
    *lost = true;  // another file under its name
  } else {
    // The header the one named: the rows must read whole, or it is the
    // card's fault, never a lost base.
    cc::Container& c = sc->frame;
    w = c.open(src, kBaseSpec, nullptr);
    if (w == Why::Ok && (c.header().generation != h.baseGen || c.section(0).count != h.baseRows)) w = Why::Counts;
    if (w == Why::Ok) w = scanRows(src, c.section(0), a);
  }
  w = ofFile(*f, w);
  fs_.close(f);
  o->why = *lost ? Why::Ok : w;
  // A read the card failed, and rows whose CRC fails (a card fault too),
  // are read again at the boot.
  return w == Why::Io || w == Why::SectionCrc ? Again::CardFault : Again::Final;
}

bool Store::quarantine() {
  // stats.bin renamed stats.bad, an older stats.bad removed first. Only
  // the twins are names 2.12.6's rule knows: stats.bad never shares a
  // chain, or collectTwins() or replace() would one day free clusters it
  // holds. So stats.bin goes to a twin instead when another entry holds
  // its chain (a rename cut, settled to a twin), and an older stats.bad
  // that shares one (a cut of this very rename) goes to a twin, not away.
  // A question the card fails: nothing done.
  char twins[tagstore::kMaxTwins][kNameCap];
  Fs::Entry tw[tagstore::kMaxTwins];
  const Fs::Entry bin = fs_.entry(kStatsBin);
  const Fs::Entry bad = fs_.entry(kStatsBad);
  const Fs::Entry tmp = fs_.entry(kStatsTmp);
  bool failed = bin.cardFailed || bad.cardFailed || tmp.cardFailed;
  for (int i = 0; i < tagstore::kMaxTwins && !failed; ++i) {
    if (!tagstore::twinName(kStatsNames, i + 1, twins[i], kNameCap)) return false;
    tw[i] = fs_.entry(twins[i]);
    failed = tw[i].cardFailed;
  }
  if (failed) return false;
  if (!bin.there) return true;  // (gone meanwhile)
  auto twinOn = [&](uint32_t c) {
    for (const Fs::Entry& e : tw)
      if (e.there && e.firstCluster == c) return true;
    return false;
  };
  auto toTwin = [&](const char* from, uint32_t c) {
    for (int i = 0; i < tagstore::kMaxTwins; ++i) {
      if (tw[i].there) continue;
      if (!fs_.rename(from, twins[i])) return false;
      tw[i].there = true;
      tw[i].firstCluster = c;
      return true;
    }
    return false;  // every twin name taken: a disk check frees them
  };
  const uint32_t c = bin.firstCluster;
  const bool badOnBin = bad.there && c != 0 && bad.firstCluster == c;
  if (c != 0 && ((tmp.there && tmp.firstCluster == c) || twinOn(c) || badOnBin)) {
    // Another entry on stats.bin's chain: stats.bin to a twin, and a
    // stats.bad on it too (the twins then hold it until a disk check).
    if (badOnBin && !toTwin(kStatsBad, c)) return false;
    return toTwin(kStatsBin, c);
  }
  if (bad.there) {
    const uint32_t d = bad.firstCluster;
    const bool shared = d != 0 && ((tmp.there && tmp.firstCluster == d) || twinOn(d));
    if (shared ? !toTwin(kStatsBad, d) : !fs_.remove(kStatsBad)) return false;
  }
  return fs_.rename(kStatsBin, kStatsBad);
}

Opened Store::read(sf::Fold& fold, uint8_t* work, WaitFn wait) {
  Opened o;
  on_ = changed_ = false;
  expect_ = Copy();
  fold.clear();
  if (!work) {
    o.read = Read::NoMemory;
    return o;
  }
  Scratch* sc = new (work) Scratch;
  uint8_t* a = work + kScratchBytes;
  uint8_t* b = a + kBufBytes;
  // A tmp this session wrote whole and holds (its replace() failed): it
  // takes stats.bin's place first, as the next save would have (settle()
  // would drop it while stats.bin is there). The card failing that: Unread,
  // and the tmp still held.
  if (held_.present) {
    const Promote pr = promoteHeld();
    if (pr == Promote::Fault) {
      o.why = Why::Io;
      return o;
    }
    o.heldPromoted = pr == Promote::Promoted;
    held_ = Copy();
  }
  StatsCheck check(a, kBufBytes);
  uint32_t waited = 0;
  bool again = false;
  for (;;) {
    ++o.reads;
    o.settled = tagstore::settle(fs_, kStatsNames, &check);
    Again g;
    if (o.settled.what == tagstore::Settle::Unread || o.settled.what == tagstore::Settle::Kept) {
      // stats.tmp unsettled (the card failed a question, or the check, or the
      // rename): stats.bin may be only the older copy.
      o.read = Read::Unread;
      o.why = check.why() == Why::FatRecords ? Why::FatRecords : Why::Io;
      g = o.why == Why::Io ? Again::CardFault : Again::Final;
    } else {
      g = readStats(fold, sc, a, b, &o);
    }
    if (g == Again::CardFault && wait && waited < kWaits) {
      wait(tagstore::kBootWaitsMs[waited++]);
      continue;
    }
    if (g == Again::Corrupt && wait && !again) {
      again = true;
      wait(tagstore::kBootWaitsMs[0]);
      continue;
    }
    break;
  }
  if (o.read == Read::Corrupt) {
    // Twice at the boot: stats.bad, and counting starts again. At a remount
    // (one read) it is left for the next boot's reads.
    if (!wait || !quarantine()) {
      fold.clear();
      o.read = Read::Unread;
      return o;
    }
    o.quarantined = true;
    fold.clear();
  }
  if (o.read == Read::Loaded) {
    expect_.present = true;
    expect_.generation = o.generation;
    expect_.headerCrc = o.headerCrc;
    const sf::Head h = fold.head();
    expect_.baseGen = h.baseGen;  // the name stats.bin names, whole or lost: no compaction writes it
    if (h.baseGen) {
      bool lost = false;
      uint32_t w = 0;
      for (;;) {
        ++o.baseReads;
        const Again g = checkBase(h, sc, a, &o, &lost);
        if (g == Again::CardFault && wait && w < kWaits) {
          wait(tagstore::kBootWaitsMs[w++]);
          continue;
        }
        break;
      }
      if (lost) {
        fold.baseLost();
        o.baseLost = true;
      } else if (o.why != Why::Ok) {
        fold.clear();
        expect_ = Copy();
        o.read = Read::Unread;
        o.inBase = true;
        return o;
      } else {
        // Checked whole: a cut compaction's neighbours go.
        char n[kNameCap];
        if (h.baseGen > 1 && baseName(h.baseGen - 1, n, sizeof(n)) && fs_.remove(n)) ++o.removed;
        if (h.baseGen < 0xFFFFFFFFu && baseName(h.baseGen + 1, n, sizeof(n)) && fs_.remove(n)) ++o.removed;
      }
    }
  }
  on_ = statsOn(o.read);
  return o;
}

// ---------------------------------------------------------------------------
// The save.
// ---------------------------------------------------------------------------
Store::Hdr Store::headerOf(const char* path, Copy* out) {
  *out = Copy();
  bool cardFailed = false;
  File* f = fs_.openRead(path, &cardFailed);
  if (!f) return !cardFailed && fs_.look(path) == Fs::Seen::Gone ? Hdr::None : Hdr::Fault;
  out->present = true;
  Hdr h = Hdr::Short;
  if (f->size() >= kHeaderBytes) {
    uint8_t b[kHeaderBytes];
    if (f->read(0, b, kHeaderBytes)) {
      out->generation = cc::get32(b + 20);
      out->headerCrc = cc::get32(b + 32);
      h = Hdr::Whole;
    } else {
      h = Hdr::Fault;
    }
  }
  fs_.close(f);
  return h;
}

Store::Promote Store::promoteHeld() {
  // QueueStore::settleTemp()'s steps, for a tmp whose header is still the
  // one this session wrote (another player may have been at the card).
  const Fs::Entry t = fs_.entry(kStatsTmp);
  if (t.cardFailed) return Promote::Fault;
  if (!t.there) return Promote::Gone;  // (its rename done after all, or another's settle() took it)
  Copy tc;
  const Hdr h = headerOf(kStatsTmp, &tc);
  if (h == Hdr::Fault) return Promote::Fault;
  if (h != Hdr::Whole || tc.generation != held_.generation || tc.headerCrc != held_.headerCrc) return Promote::NotOurs;
  const Fs::Entry f = fs_.entry(kStatsBin);
  if (f.cardFailed) return Promote::Fault;
  // stats.bin on the tmp's chain: the rename was cut between its two
  // writes, stats.bin holds it; the tmp goes to a twin (a rename frees
  // nothing).
  const bool oneChain = f.there && t.firstCluster != 0 && f.firstCluster == t.firstCluster;
  if (oneChain ? !tagstore::prepareTmp(fs_, kStatsNames) : !tagstore::replace(fs_, kStatsNames)) return Promote::Fault;
  expect_ = held_;
  return Promote::Promoted;
}

// Its own frame (the old base's Container and Stream, the merge's rows),
// never inlined into save(): a rotation's save, the loop's deepest path,
// never compacts.
__attribute__((noinline)) bool Store::compact(sf::Fold& fold, Scratch* sc, uint8_t* a, uint8_t* b, SaveReport* r,
                                              uint32_t* oldGen) {
  sf::Fold::Plan* p = &sc->plan;
  const uint32_t from = p->head.baseGen;
  const uint32_t to = from + 1;
  // Never under the name the card's stats.bin names (5.3): after a base
  // lost, baseGen starts again at 0 and 1 may be the lost one's name; after
  // a held tmp that compacted, promoted, it is the held base's. A cut after
  // the create would leave a base shorter than its header there, which
  // every later boot reads as Unread (stats off for good); a whole base
  // would leave the card's stats.bin naming another file. The next boot's
  // save compacts (its stats.bin names another base, or none).
  if (from == 0xFFFFFFFFu || (expect_.baseGen != 0 && expect_.baseGen == to)) {
    r->compactSkipped = true;
    return false;
  }
  char name[kNameCap];
  baseName(to, name, sizeof(name));
  // The old base, checked as it passes: its header the one named, its
  // rows' count, order and CRC.
  File* ob = nullptr;
  tagstore::ReadWatch src;
  cc::Container& c = sc->frame;
  cc::Stream& st = sc->stream;
  uint32_t left = 0, stride = 0, oldCrc = 0;
  bool ok = true;
  if (from) {
    char old[kNameCap];
    baseName(from, old, sizeof(old));
    bool cardFailed = false;
    ob = fs_.openRead(old, &cardFailed);
    ok = ob != nullptr;
    if (ok) {
      src.watch(ob);
      ok = c.open(src, kBaseSpec, nullptr) == Why::Ok && c.header().headerCrc == p->head.baseCrc &&
           c.header().generation == from && c.section(0).count == p->head.baseRows;
    }
    if (ok) {
      const cc::Section& s = c.section(0);
      st.begin(&src, s.offset, s.offset + s.bytes, a, kBufBytes);
      left = s.count;
      stride = s.stride;
      oldCrc = s.crc;
    }
  }
  // The new base, under a name stats.bin doesn't name: a stale file there
  // (a stats.bin deleted or corrupt, a base lost) is replaced.
  File* nb = ok ? fs_.open(name, Fs::Mode::Create) : nullptr;
  ok = ok && nb;
  uint32_t n = 0, crc = 0, headerCrc = 0;
  if (ok) {
    PieceSink sink(*nb, b);
    bool oldBad = false;
    uint64_t lastOld = 0;
    sf::Track d, o;
    auto readOld = [&]() {
      if (!left || oldBad) return false;
      uint8_t row[sf::kTrackBytes];
      if (!st.read(row, sf::kTrackBytes) || !st.read(nullptr, stride - sf::kTrackBytes)) {
        oldBad = true;
        return false;
      }
      o = sf::decodeTrack(row);
      if (left != c.section(0).count && o.hash <= lastOld) oldBad = true;
      lastOld = o.hash;
      --left;
      return !oldBad;
    };
    fold.rowsBegin(*p, sf::kTrkd);
    bool more = fold.nextTrack(&d);
    bool haveOld = from && readOld();
    uint8_t row[sf::kTrackBytes];
    while (ok && !oldBad && (more || haveOld)) {
      sf::Track t;
      if (more && (!haveOld || d.hash < o.hash)) {
        t = d;
        more = fold.nextTrack(&d);
      } else if (!more || o.hash < d.hash) {
        t = o;
        haveOld = readOld();
      } else {
        t = sf::merge(o, d);  // the base the older
        haveOld = readOld();
        more = fold.nextTrack(&d);
      }
      sf::encodeTrack(t, row);
      ok = sink.write(kBaseRowsAt + n * sf::kTrackBytes, row, sf::kTrackBytes);
      crc = cc::crc32(row, sizeof(row), crc);
      ++n;
    }
    ok = ok && !oldBad && (!from || (left == 0 && st.crc() == oldCrc));
    uint8_t frame[kBaseRowsAt];
    baseFrame(to, n, crc, frame, &headerCrc);
    ok = ok && sink.write(0, frame, kBaseRowsAt) && sink.flush();
    ok = fs_.close(nb) && ok;
  }
  if (ob) fs_.close(ob);
  if (ok) {
    // Its size on the card, against what was written.
    bool cardFailed = false;
    File* chk = fs_.openRead(name, &cardFailed);
    ok = chk && chk->size() == kBaseRowsAt + n * sf::kTrackBytes;
    if (chk) fs_.close(chk);
  }
  if (!ok) {
    if (nb) fs_.remove(name);  // (named by nothing: a later boot would remove it anyway)
    r->compactFailed = true;
    return false;
  }
  *oldGen = from;
  p->head.baseGen = to;
  p->head.baseRows = n;
  p->head.baseCrc = headerCrc;
  p->compact = true;
  r->compacted = true;
  r->baseRows = n;
  r->baseBytes = kBaseRowsAt + n * sf::kTrackBytes;
  return true;
}

SaveReport Store::save(sf::Fold& fold, const SaveAsk& ask, uint8_t* work) {
  SaveReport r;
  // The boot's fold ends here whatever the save does (plan()'s first step,
  // which a refused save never reaches).
  fold.endBoot();
  if (!on_) {
    r.what = Saved::Off;
    return r;
  }
  if (changed_) {
    r.what = Saved::Changed;
    return r;
  }
  if (!work) {
    r.what = fold.dirty() ? Saved::NoMemory : Saved::Nothing;
    return r;
  }
  Scratch* sc = new (work) Scratch;
  uint8_t* a = work + kScratchBytes;
  uint8_t* b = a + kBufBytes;
  sf::Fold::Plan& p = sc->plan;
  if (!fold.plan(ask.nowMs, ask.hasCommit, ask.commitSec, &p)) {
    r.what = Saved::Off;  // a partial scan, or the fold out of memory
    return r;
  }
  if (!fold.dirty()) {
    r.what = Saved::Nothing;
    return r;
  }
  auto failed = [&r](const char* step) {
    r.what = Saved::Failed;
    r.step = step;
    return r;
  };

  // 1. A held tmp promoted first; settle() with the real check; the card's
  // copy against the one this RAM stands on; prepareTmp().
  bool either = false;
  Copy alt;
  if (held_.present) {
    const Promote pr = promoteHeld();
    if (pr == Promote::Fault) return failed("held tmp");
    r.heldPromoted = pr == Promote::Promoted;
    if (pr == Promote::Gone) {
      either = true;  // stats.bin is the held copy, or the one before it
      alt = held_;
    }
    held_ = Copy();
  }
  StatsCheck check(a, kBufBytes);
  const tagstore::Settle st = tagstore::settle(fs_, kStatsNames, &check).what;
  if (st == tagstore::Settle::Unread || st == tagstore::Settle::Kept || st == tagstore::Settle::Failed)
    return failed("settle");
  Copy got;
  const Hdr h = headerOf(kStatsBin, &got);
  if (h == Hdr::Fault) return failed("card copy");
  const bool asAlt = either && h != Hdr::Short && same(got, alt);
  if (h == Hdr::Short || (!same(got, expect_) && !asAlt)) {
    changed_ = true;
    r.what = Saved::Changed;
    return r;
  }
  if (asAlt) expect_ = alt;
  if (!tagstore::prepareTmp(fs_, kStatsNames)) return failed("prepare");
  // The card's copy's next generation (a held tmp promoted was a save).
  if (expect_.present && p.generation <= expect_.generation && expect_.generation != 0xFFFFFFFFu)
    p.generation = expect_.generation + 1;

  // 2. Compaction, when asked and due (a failure leaves the base as it was).
  uint32_t oldGen = 0;
  if (ask.mayCompact && sf::compactionDue(p.rows[sf::kTrkd], p.head.baseRows, ask.playing, ask.safePoint))
    compact(fold, sc, a, b, &r, &oldGen);

  // 3. stats.tmp through the writer, in 4 KB pieces.
  File* t = fs_.open(kStatsTmp, Fs::Mode::Create);
  if (!t) return failed("tmp");
  cc::ContainerWriter* cw = &sc->writer;
  PieceSink sink(*t, b);
  cc::SectionOut* s = sc->sections;
  for (uint32_t i = 0; i < sf::kSecs; ++i) {
    const sf::Sec sec = static_cast<sf::Sec>(i);
    s[i].type = sf::kTypes[i];
    s[i].flags = cc::kSectionRequired;
    s[i].stride = sf::kStrides[i];
    s[i].count = sec == sf::kHead ? 0 : p.rowsOf(sec);
    s[i].bytes = p.bytesOf(sec);
  }
  cc::FileMeta meta;
  meta.magic = kMagic;
  meta.major = kMajor;
  meta.minor = kMinor;
  meta.generation = p.generation;
  meta.cardId = 0;
  bool ok = cw->begin(sink, meta, nullptr, kHeaderBytes, s, sf::kSecs);
  sf::encodeHead(p.head, a);
  ok = ok && cw->write(a, sf::kHeadBytes);
  for (uint32_t i = 1; ok && i < sf::kSecs; ++i) {
    const sf::Sec sec = static_cast<sf::Sec>(i);
    if (!p.rowsOf(sec)) continue;
    fold.rowsBegin(p, sec);
    const uint32_t per = kBufBytes / sf::kStrides[i];
    for (uint32_t k; ok && (k = fold.nextRows(a, per)) > 0;) ok = cw->write(a, k * sf::kStrides[i]);
  }
  uint32_t fileBytes = 0, headerCrc = 0;
  ok = ok && cw->finish(&fileBytes, &headerCrc) && sink.flush();
  if (!fs_.close(t) || !ok) {
    fs_.remove(kStatsTmp);
    return failed("tmp");
  }
  // 4. Its size on the card, against what the writer wrote.
  bool cardFailed = false;
  File* chk = fs_.openRead(kStatsTmp, &cardFailed);
  const bool whole = chk && chk->size() == fileBytes;
  if (chk) fs_.close(chk);
  if (!whole) {
    fs_.remove(kStatsTmp);
    return failed("size");
  }
  r.generation = p.generation;
  r.bytes = fileBytes;
  // 5. replace(): false keeps the tmp, whole, held for the next save.
  held_.present = true;
  held_.generation = p.generation;
  held_.headerCrc = headerCrc;
  held_.baseGen = p.head.baseGen;
  if (!tagstore::replace(fs_, kStatsNames)) {
    r.what = Saved::Held;
    r.step = "replace";
    return r;
  }
  // 6. The card's copy is this one: RAM takes the plan, and an old base goes.
  expect_ = held_;
  held_ = Copy();
  fold.saved(p);
  if (r.compacted && oldGen) {
    char old[kNameCap];
    r.oldBaseRemoved = baseName(oldGen, old, sizeof(old)) && fs_.remove(old);
  }
  r.what = Saved::Wrote;
  return r;
}

}  // namespace statsfile
