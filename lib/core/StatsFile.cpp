// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "StatsFile.h"

#include <cstdio>
#include <cstdlib>
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
constexpr uint32_t kNoDay = 0x10000;  // RCNT's first day kept: none (every row cut)

static_assert(kReadBytes >= 2 * cc::Stream::kSector, "a read stream refills whole sectors");
static_assert(kReadBytes >= sf::kHeadBytes && kReadBytes / sf::kStrides[sf::kMark] > 0, "HEAD's and MARK's staging");
static_assert(sizeof(Scratch::row) >= kBaseRowsAt, "a base's frame in the merge's rows");

void* mallocAlloc(size_t n) { return std::malloc(n); }
void mallocFree(void* p) { std::free(p); }

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

bool same(const Store::Copy& a, const Store::Copy& b) {
  return a.present == b.present && (!a.present || (a.generation == b.generation && a.headerCrc == b.headerCrc));
}

// One section's rows merged: the card's (CardRows, the older) and the
// records' (a Cursor, a key's records ⊕'d into one row), by compareRows(),
// equal keys ⊕'d by addRow(). Both sides strictly increasing, so the merge
// is too. `c` and `r` hold each side's next row.
class Merge {
public:
  Merge(CardRows& card, sf::Fold::Cursor& rec, sf::Sec sec, uint8_t* c, uint8_t* r)
      : card_(card), rec_(rec), sec_(sec), c_(c), r_(r) {
    hc_ = card_.next(c_);
    hr_ = rec_.next(r_);
  }
  bool next(uint8_t* out) {
    if (!hc_ && !hr_) return false;
    const int k = !hc_ ? 1 : !hr_ ? -1 : sf::compareRows(sec_, c_, r_);
    if (k <= 0) {
      std::memcpy(out, c_, sf::kStrides[sec_]);
      if (k == 0) sf::addRow(sec_, out, r_);
      hc_ = card_.next(c_);
    } else {
      std::memcpy(out, r_, sf::kStrides[sec_]);
    }
    if (k >= 0) hr_ = rec_.next(r_);
    return true;
  }

private:
  CardRows& card_;
  sf::Fold::Cursor& rec_;
  sf::Sec sec_;
  uint8_t* c_;
  uint8_t* r_;
  bool hc_ = false, hr_ = false;
};

// stats.bin's sections in one pass (5.4, 5.14), each section's CRC taken as
// it streams: HEAD (its first 128 bytes) and MARK (in batches through
// `stage`, by their prefix) into the Fold; HOUR to TRKD only checked, their
// rows' order by compareRows(), none kept. Every CRC before the order, as
// statsref's read: SectionCrc, then Shape (HEAD under 128 bytes), then
// Order; a read the card failed at once (Io). MARK's rows past markCap():
// *noMemory.
Why loadAll(sf::Fold& fold, cc::Source& src, const cc::Container& c, CardRows& rows, uint8_t* row, uint8_t* buf,
            uint8_t* stage, bool* noMemory) {
  bool crc = true, shape = false, order = true;
  for (uint32_t i = 0; i < sf::kSecs; ++i) {
    const cc::Section& s = c.section(i);
    const sf::Sec sec = static_cast<sf::Sec>(i);
    if (sec >= sf::kHour) {
      rows.begin(&src, s, sec, buf, kReadBytes);
      while (rows.next(row)) {
      }
      const Why w = rows.end();
      if (w == Why::Io) return Why::Io;
      if (w == Why::SectionCrc) crc = false;
      if (w == Why::Order) order = false;
      continue;
    }
    cc::Stream st;
    st.begin(&src, s.offset, s.offset + s.bytes, buf, kReadBytes);
    if (sec == sf::kHead) {
      if (s.bytes < sf::kHeadBytes) {
        shape = true;
      } else if (!st.read(stage, sf::kHeadBytes)) {
        return Why::Io;
      } else {
        fold.loadHead(stage);
      }
    } else {
      const uint32_t per = kReadBytes / s.stride;  // (a MARK row over 2 KB: no minor writes one)
      bool load = per > 0;
      shape = shape || per == 0;
      for (uint32_t left = per ? s.count : 0; left;) {
        const uint32_t k = left < per ? left : per;
        if (!st.read(stage, k * s.stride)) return Why::Io;
        if (load && !fold.loadMark(stage, k, s.stride)) {
          load = false;
          if (fold.noMemory()) {
            *noMemory = true;
          } else {
            order = false;
          }
        }
        left -= k;
      }
    }
    if (!st.drain()) return Why::Io;
    if (st.crc() != s.crc) crc = false;
  }
  if (!crc) return Why::SectionCrc;
  if (shape) return Why::Shape;
  return order ? Why::Ok : Why::Order;
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

// ---------------------------------------------------------------------------
// A section's rows.
// ---------------------------------------------------------------------------
void CardRows::begin(cc::Source* src, const cc::Section& s, sf::Sec sec, uint8_t* buf, uint32_t bytes) {
  sec_ = sec;
  present_ = src != nullptr && s.present();
  left_ = present_ ? s.count : 0;
  stride_ = s.stride;
  crc_ = s.crc;
  n_ = 0;
  order_ = true;
  failed_ = false;
  if (present_) st_.begin(src, s.offset, s.offset + s.bytes, buf, bytes);
}

bool CardRows::next(uint8_t* row) {
  if (!present_ || failed_ || !order_ || left_ == 0) return false;
  const uint32_t k = sf::kStrides[sec_];
  if (!st_.read(row, k) || (stride_ > k && !st_.read(nullptr, stride_ - k))) {
    failed_ = true;
    return false;
  }
  if (n_ && sf::compareRows(sec_, last_, row) >= 0) {
    order_ = false;
    return false;
  }
  std::memcpy(last_, row, k);
  ++n_;
  --left_;
  return true;
}

Why CardRows::end() {
  if (!present_) return Why::Ok;
  if (!failed_ && !st_.drain()) failed_ = true;
  if (failed_) return st_.readFailed() ? Why::Io : Why::Shape;
  if (st_.crc() != crc_) return Why::SectionCrc;
  return order_ ? Why::Ok : Why::Order;
}

// ---------------------------------------------------------------------------
// settle()'s check, the names of the outcomes.
// ---------------------------------------------------------------------------
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
    case Saved::Behind: return "behind";
    case Saved::Damaged: return "damaged";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// The store.
// ---------------------------------------------------------------------------
Store::Store(Fs& fs, AllocFn alloc, FreeFn release)
    : fs_(fs), alloc_(alloc ? alloc : mallocAlloc), free_(release ? release : mallocFree) {}

Store::~Store() {
  if (mark_) free_(mark_);
}

// ---------------------------------------------------------------------------
// The read.
// ---------------------------------------------------------------------------
Store::Again Store::readStats(sf::Fold& fold, Scratch* sc, uint8_t* buf, uint8_t* stage, bool carve, Opened* o) {
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
  bool noMemory = false;
  if (w == Why::Ok) {
    // MARK's room (5.14): the object's 16 rows, or a block of the card's
    // rows and kMarkRoom more, carved once at the boot (in setup, kept for
    // the session); a remount carves nothing: MARK past the room is a read
    // with no memory, never a MARK cut short.
    const uint32_t marks = c.section(sf::kMark).count;
    o->marks = marks;
    if (carve && marks + sf::kMarkRoom > fold.markCap()) {
      const size_t bytes = sf::Fold::markBlockBytes(marks);
      void* m = alloc_(bytes);
      if (!m || !fold.markBlock(m, bytes)) {
        if (m) free_(m);
        noMemory = true;
      } else {
        if (mark_) free_(mark_);  // (a block for fewer rows: the Fold uses the new one)
        mark_ = m;
        markBytes_ = bytes;
        o->markBytes = static_cast<uint32_t>(bytes);
      }
    } else if (marks > fold.markCap()) {
      noMemory = true;
    }
    if (!noMemory) {
      fold.loadGeneration(c.header().generation);
      w = loadAll(fold, src, c, sc->card, sc->row[0], buf, stage, &noMemory);
    }
  }
  w = ofFile(*f, w);
  fs_.close(f);
  if (noMemory && w != Why::Io && w != Why::FatRecords) {
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

Store::Again Store::checkBase(const sf::Head& h, Scratch* sc, uint8_t* buf, Opened* o, bool* lost) {
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
    // The header the one named: the rows must read whole (their CRC, then
    // their order), or it is the card's fault, never a lost base.
    cc::Container& c = sc->base;
    w = c.open(src, kBaseSpec, nullptr);
    if (w == Why::Ok && (c.header().generation != h.baseGen || c.section(0).count != h.baseRows)) w = Why::Counts;
    if (w == Why::Ok) {
      sc->old.begin(&src, c.section(0), sf::kTrkd, buf, kReadBytes);
      while (sc->old.next(sc->row[0])) {
      }
      w = sc->old.end();
    }
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
  uint8_t* w = work + kScratchBytes;
  uint8_t* r1 = w + kBufBytes;
  uint8_t* r2 = r1 + kReadBytes;
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
  StatsCheck check(w, kBufBytes);
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
      g = readStats(fold, sc, r1, r2, wait != nullptr, &o);
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
      uint32_t k = 0;
      for (;;) {
        ++o.baseReads;
        const Again g = checkBase(h, sc, r1, &o, &lost);
        if (g == Again::CardFault && wait && k < kWaits) {
          wait(tagstore::kBootWaitsMs[k++]);
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
  if (!t.there) return Promote::Gone;  // (its rename done after all, or another's settle() took it, or lost)
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

Store::Held Store::promote(sf::Fold& fold) {
  // Only where save() would come to its step 1: the stats on, the card's
  // copy unchanged, a fold that may be saved (the boot's fold ended first,
  // as save() ends it). Whatever is new since: the held tmp is something
  // new itself (statsref's _promote()).
  fold.endBoot();
  if (!held_.present || !on_ || changed_ || fold.partialScan() || fold.noMemory() || fold.behind()) return Held::None;
  const Promote pr = promoteHeld();
  if (pr == Promote::Fault) return Held::Fault;  // (the tmp still held)
  if (pr == Promote::Promoted) {
    held_ = Copy();
    fold.promoted();
    return Held::Promoted;
  }
  if (pr == Promote::Gone) {
    // stats.bin the held copy (its rename done after all, or another's
    // settle() took it): promoted. The copy before it: the tmp lost (a PC,
    // another player's settle() with stats.bin there), so RAM no longer
    // holds the slots it took: behind. Another: changed.
    Copy got;
    const Hdr h = headerOf(kStatsBin, &got);
    if (h == Hdr::Fault) return Held::Fault;
    if (h == Hdr::Whole && same(got, held_)) {
      expect_ = held_;
      held_ = Copy();
      fold.promoted();
      return Held::Promoted;
    }
    held_ = Copy();
    if (h != Hdr::Short && same(got, expect_)) {
      fold.fallBehind();
      return Held::Behind;
    }
    changed_ = true;
    return Held::Changed;
  }
  // Another's tmp under its name (left to settle()): the held copy is gone.
  held_ = Copy();
  fold.fallBehind();
  return Held::Behind;
}

// One section's rows merged (the card's and the records'), front to back:
// counted (Count: from RCNT's day `from` on; the last row's day), RCNT's
// whole-day cut found (Cut: the first day from which `total` less the rows
// before it is at most kRcntRows), or written to the tmp (Write: the cut's
// rows dropped first, then the plan's count). The card's rows' end:
// Ok, Io, SectionCrc or Order.
Why Store::pass(sf::Fold& fold, Scratch* sc, cc::Source* bin, sf::Sec sec, Pass how, uint32_t from, uint32_t total,
                uint8_t* buf, Tally* t) {
  *t = Tally();
  t->cutDay = kNoDay;
  CardRows& card = sc->card;
  card.begin(bin, bin ? sc->frame.section(sec) : cc::Section(), sec, buf, kReadBytes);
  sf::Fold::Cursor* cur = new (sc->cursor) sf::Fold::Cursor(fold, sec);
  Merge m(card, *cur, sec, sc->row[0], sc->row[1]);
  uint8_t* row = sc->row[2];
  const bool dayKey = sec == sf::kDays || sec == sf::kRcnt;
  const uint32_t drop = how == Pass::Write ? sc->drop[sec] : 0;
  const uint32_t want = how == Pass::Write ? sc->plan.rowsOf(sec) : 0;
  uint32_t seen = 0, day = kNoDay;
  bool found = false;
  while (m.next(row)) {
    const uint32_t d = dayKey ? cc::get16(row) : 0;
    if (sec == sf::kRcnt && d < from) continue;
    if (how == Pass::Count) {
      ++t->rows;
      t->lastDay = d;
    } else if (how == Pass::Cut) {
      if (!found && d != day) {
        if (total - seen <= sf::kRcntRows) {
          found = true;
          t->cutDay = d;
          t->cutRows = total - seen;
        }
        day = d;
      }
      ++seen;
    } else if (seen++ >= drop) {
      if (t->rows == want) {
        t->more = true;
      } else if (!sc->writer.write(row, sf::kStrides[sec])) {
        t->wrote = false;
        return Why::Ok;  // (the tmp's: the caller says so)
      } else {
        ++t->rows;
      }
    }
  }
  return card.end();
}

// The counting pass (5.14 step 3): HEAD's and MARK's CRCs (RAM writes them:
// a damaged one is the card's copy damaged all the same), then HOUR, DAYS,
// RCNT and TRKD merged with the records, the rows each keeps after its cut
// into the plan. `*failedSec`: the section a check failed in.
Why Store::count(sf::Fold& fold, Scratch* sc, cc::Source* bin, const SaveAsk& ask, uint8_t* buf, uint32_t* failedSec) {
  sf::Fold::Plan& p = sc->plan;
  for (uint32_t i = sf::kHead; bin && i <= sf::kMark; ++i) {
    const cc::Section& s = sc->frame.section(i);
    cc::Stream st;
    st.begin(bin, s.offset, s.offset + s.bytes, buf, kReadBytes);
    *failedSec = i;
    if (!st.drain()) return Why::Io;
    if (st.crc() != s.crc) return Why::SectionCrc;
  }
  Tally t;
  Why w = Why::Ok;
  // HOUR and DAYS: their newest 840 and 400 rows, never by date.
  for (const sf::Sec sec : {sf::kHour, sf::kDays}) {
    *failedSec = sec;
    if ((w = pass(fold, sc, bin, sec, Pass::Count, 0, 0, buf, &t)) != Why::Ok) return w;
    const uint32_t cap = sec == sf::kHour ? sf::kHourRows : sf::kDaysRows;
    p.rows[sec] = t.rows < cap ? t.rows : cap;
    sc->drop[sec] = t.rows - p.rows[sec];
  }
  // RCNT: 32 days back from the earlier of today and DAYS' newest merged day
  // (nothing by date with the clock unset or no day), then whole days,
  // oldest first, while more than 4,096 rows are left (a day of more than
  // 4,096 goes whole too).
  const bool anyDay = t.rows > 0;
  const uint32_t newest = t.lastDay;
  uint32_t from = 0;
  uint16_t today = 0;
  if (sf::todayOf(ask.nowMs, &today) && anyDay) {
    const uint32_t ref = today < newest ? today : newest;
    from = ref + 1 > sf::kRcntDays ? ref + 1 - sf::kRcntDays : 0;
  }
  *failedSec = sf::kRcnt;
  if ((w = pass(fold, sc, bin, sf::kRcnt, Pass::Count, from, 0, buf, &t)) != Why::Ok) return w;
  uint32_t rows = t.rows;
  if (rows > sf::kRcntRows) {
    const uint32_t total = rows;
    if ((w = pass(fold, sc, bin, sf::kRcnt, Pass::Cut, from, total, buf, &t)) != Why::Ok) return w;
    from = t.cutDay;
    rows = t.cutRows;
  }
  sc->rcntFrom = from;
  p.rows[sf::kRcnt] = rows;
  sc->drop[sf::kRcnt] = 0;
  // TRKD: never cut.
  *failedSec = sf::kTrkd;
  if ((w = pass(fold, sc, bin, sf::kTrkd, Pass::Count, 0, 0, buf, &t)) != Why::Ok) return w;
  p.rows[sf::kTrkd] = t.rows;
  sc->drop[sf::kTrkd] = 0;
  return Why::Ok;
}

// Its own frame, never inlined into save(): only the boot's save compacts.
// base ⊕ TRKD ⊕ the records (5.3, 5.14): the old base (its header the one
// named, its rows' count, order and CRC as they pass), stats.bin's TRKD
// (its order and CRC) merged with the records first, the older side kept
// first on equal hashes; written to stats-<gen+1>.bin, rows from offset 72,
// the frame last (the count isn't known before the merge).
__attribute__((noinline)) bool Store::compact(sf::Fold& fold, Scratch* sc, cc::Source* bin, uint8_t* w, uint8_t* r1,
                                              uint8_t* r2, SaveReport* r, uint32_t* oldGen) {
  sf::Fold::Plan* p = &sc->plan;
  const uint32_t from = p->head.baseGen;
  const uint32_t to = from + 1;
  // Never under the name the card's stats.bin names (5.3): after a base
  // lost, baseGen starts again at 0 and 1 may be the lost one's name. A cut
  // after the create would leave a base shorter than its header there,
  // which every later boot reads as Unread (stats off for good). The next
  // boot's save compacts (its stats.bin names another base, or none).
  if (from == 0xFFFFFFFFu || (expect_.baseGen != 0 && expect_.baseGen == to)) {
    r->compactSkipped = true;
    return false;
  }
  char name[kNameCap];
  baseName(to, name, sizeof(name));
  File* ob = nullptr;
  tagstore::ReadWatch src;
  CardRows& old = sc->old;
  old.begin(nullptr, cc::Section(), sf::kTrkd, r2, kReadBytes);
  bool ok = true;
  if (from) {
    char oldName[kNameCap];
    baseName(from, oldName, sizeof(oldName));
    bool cardFailed = false;
    ob = fs_.openRead(oldName, &cardFailed);
    ok = ob != nullptr;
    if (ok) {
      src.watch(ob);
      cc::Container& c = sc->base;
      ok = c.open(src, kBaseSpec, nullptr) == Why::Ok && c.header().headerCrc == p->head.baseCrc &&
           c.header().generation == from && c.section(0).count == p->head.baseRows;
      if (ok) old.begin(&src, c.section(0), sf::kTrkd, r2, kReadBytes);
    }
  }
  // The new base, under a name stats.bin doesn't name: a stale file there
  // (a stats.bin deleted or corrupt, a base lost) is replaced.
  File* nb = ok ? fs_.open(name, Fs::Mode::Create) : nullptr;
  ok = ok && nb;
  uint32_t n = 0, crc = 0, headerCrc = 0;
  if (ok) {
    PieceSink sink(*nb, w);
    CardRows& card = sc->card;
    card.begin(bin, bin ? sc->frame.section(sf::kTrkd) : cc::Section(), sf::kTrkd, r1, kReadBytes);
    sf::Fold::Cursor* cur = new (sc->cursor) sf::Fold::Cursor(fold, sf::kTrkd);
    Merge delta(card, *cur, sf::kTrkd, sc->row[0], sc->row[1]);
    uint8_t* d = sc->row[2];
    uint8_t* b = sc->row[3];
    bool hd = delta.next(d), hb = old.next(b);
    while (ok && (hd || hb)) {
      const int k = !hb ? 1 : !hd ? -1 : sf::compareRows(sf::kTrkd, b, d);
      if (k == 0) sf::addRow(sf::kTrkd, b, d);  // the base the older
      const uint8_t* out = k <= 0 ? b : d;
      ok = sink.write(kBaseRowsAt + n * sf::kTrackBytes, out, sf::kTrackBytes);
      crc = cc::crc32(out, sf::kTrackBytes, crc);
      ++n;
      if (k <= 0) hb = old.next(b);
      if (k >= 0) hd = delta.next(d);
    }
    ok = ok && old.end() == Why::Ok && card.end() == Why::Ok;
    uint8_t* frame = &sc->row[0][0];  // (the merge done: its rows free)
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

// The writing pass (5.14 step 4): HEAD and MARK from the plan, then HOUR,
// DAYS, RCNT and TRKD (none after a compaction) merged again from the card
// with the records, each as the counting pass found it, through the
// writer in 4 KB pieces.
Store::Step Store::write(sf::Fold& fold, Scratch* sc, cc::Source* bin, File& tmp, uint8_t* w, uint8_t* r1,
                         uint8_t* r2, uint32_t* failedSec, uint32_t* fileBytes, uint32_t* headerCrc) {
  sf::Fold::Plan& p = sc->plan;
  cc::ContainerWriter* cw = &sc->writer;
  PieceSink sink(tmp, w);
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
  if (!cw->begin(sink, meta, nullptr, kHeaderBytes, s, sf::kSecs)) return Step::Tmp;
  sf::encodeHead(p.head, r2);
  if (!cw->write(r2, sf::kHeadBytes)) return Step::Tmp;
  const uint32_t per = kReadBytes / sf::kStrides[sf::kMark];
  uint32_t marks = 0;
  for (uint32_t k; (k = fold.markRows(marks, r2, per)) > 0; marks += k)
    if (!cw->write(r2, k * sf::kStrides[sf::kMark])) return Step::Tmp;
  if (marks != p.rows[sf::kMark]) return Step::Rows;
  for (uint32_t i = sf::kHour; i < sf::kSecs; ++i) {
    const sf::Sec sec = static_cast<sf::Sec>(i);
    if (sec == sf::kTrkd && p.compact) continue;  // (the base holds them)
    Tally t;
    const Why why = pass(fold, sc, bin, sec, Pass::Write, sec == sf::kRcnt ? sc->rcntFrom : 0, 0, r1, &t);
    if (!t.wrote) return Step::Tmp;
    if (why != Why::Ok) {
      *failedSec = sec;
      return why == Why::Io ? Step::Read : Step::Damaged;
    }
    if (t.more || t.rows != p.rowsOf(sec)) return Step::Rows;
  }
  if (!cw->finish(fileBytes, headerCrc) || !sink.flush()) return Step::Tmp;
  return Step::Ok;
}

SaveReport Store::save(sf::Fold& fold, const SaveAsk& ask, uint8_t* work, Held held) {
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
  // A partial scan, or a fold out of memory (plan()'s refusal, asked before
  // the work memory: a fold that ran out before its first fold isn't dirty,
  // and "nothing new" would let the trim pass plays it never folded).
  if (fold.partialScan() || fold.noMemory()) {
    r.what = Saved::Off;
    return r;
  }
  // A held tmp promoted first (promote(), before this call): lost, the
  // session behind from now (promote() said so to the fold).
  if (held == Held::Behind) {
    r.what = Saved::Behind;
    r.step = "held tmp lost";
    return r;
  }
  if (fold.behind()) {
    r.what = Saved::Behind;
    return r;
  }
  if (held == Held::Fault) {
    r.what = Saved::Failed;
    r.step = "held tmp";
    return r;
  }
  if (held == Held::Changed) {
    r.what = Saved::Changed;
    return r;
  }
  r.heldPromoted = held == Held::Promoted;
  if (!fold.dirty()) {
    r.what = Saved::Nothing;
    return r;
  }
  if (!work) {
    r.what = Saved::NoMemory;
    return r;
  }
  Scratch* sc = new (work) Scratch;
  uint8_t* w = work + kScratchBytes;
  uint8_t* r1 = w + kBufBytes;
  uint8_t* r2 = r1 + kReadBytes;
  sf::Fold::Plan& p = sc->plan;
  if (!fold.plan(ask.nowMs, ask.hasCommit, ask.commitSec, &p)) {
    r.what = Saved::Off;  // (refused above already)
    return r;
  }
  File* in = nullptr;
  auto failed = [&](const char* step) {
    if (in) fs_.close(in);
    r.what = Saved::Failed;
    r.step = step;
    return r;
  };
  // A section of the card's copy that fails its CRC (or its rows' order)
  // under the header RAM stands on: damaged under the session. No save may
  // heal it (RAM no longer holds the data): behind from now, the file as
  // it is (the next boot reads it Corrupt).
  auto damaged = [&](uint32_t sec) {
    if (in) fs_.close(in);
    fold.fallBehind();
    r.what = Saved::Damaged;
    r.step = "section";
    r.section = sec < sf::kSecs ? sf::kTypes[sec] : 0;
    return r;
  };

  // 1. settle() with the real check; the card's copy against the one this
  // RAM stands on; prepareTmp().
  StatsCheck check(w, kBufBytes);
  const tagstore::Settle st = tagstore::settle(fs_, kStatsNames, &check).what;
  if (st == tagstore::Settle::Unread || st == tagstore::Settle::Kept || st == tagstore::Settle::Failed)
    return failed("settle");
  Copy got;
  const Hdr h = headerOf(kStatsBin, &got);
  if (h == Hdr::Fault) return failed("card copy");
  if (h == Hdr::Short || !same(got, expect_)) {
    changed_ = true;
    r.what = Saved::Changed;
    return r;
  }
  if (!tagstore::prepareTmp(fs_, kStatsNames)) return failed("prepare");
  // The card's copy's next generation. (A guard: the Fold's is the card
  // copy's already, the read's, or the last save's, landed or held.)
  if (expect_.present && p.generation <= expect_.generation && expect_.generation != 0xFFFFFFFFu)
    p.generation = expect_.generation + 1;

  // 2. The card's copy opened (its frame: the header RAM stands on), then
  // the counting pass.
  tagstore::ReadWatch src;
  if (expect_.present) {
    bool cardFailed = false;
    in = fs_.openRead(kStatsBin, &cardFailed);
    if (!in) return failed("read");
    src.watch(in);
    const Why fw = ofFile(*in, sc->frame.open(src, kStatsSpec, nullptr));
    if (fw == Why::Io || fw == Why::FatRecords) return failed("read");
    if (fw != Why::Ok) return damaged(sf::kSecs);
    if (sc->frame.header().generation != expect_.generation || sc->frame.header().headerCrc != expect_.headerCrc) {
      fs_.close(in);
      changed_ = true;
      r.what = Saved::Changed;
      return r;
    }
  }
  cc::Source* bin = in ? &src : nullptr;
  uint32_t sec = sf::kSecs;
  const Why cw = count(fold, sc, bin, ask, r1, &sec);
  if (cw == Why::Io) return failed("read");
  if (cw != Why::Ok) return damaged(sec);

  // 3. A compaction, when asked and due on the merged TRKD's count (a
  // failure leaves the base as it was).
  uint32_t oldGen = 0;
  if (ask.mayCompact && sf::compactionDue(p.rows[sf::kTrkd], p.head.baseRows, ask.playing, ask.safePoint))
    compact(fold, sc, bin, w, r1, r2, &r, &oldGen);

  // 4. stats.tmp: the writing pass.
  File* t = fs_.open(kStatsTmp, Fs::Mode::Create);
  if (!t) return failed("tmp");
  uint32_t fileBytes = 0, headerCrc = 0;
  const Step ws = write(fold, sc, bin, *t, w, r1, r2, &sec, &fileBytes, &headerCrc);
  if (in) {
    fs_.close(in);
    in = nullptr;
  }
  const bool closed = fs_.close(t);
  if (ws != Step::Ok || !closed) {
    fs_.remove(kStatsTmp);
    if (ws == Step::Damaged) return damaged(sec);
    return failed(ws == Step::Read ? "read" : ws == Step::Rows ? "rows" : "tmp");
  }
  // 5. Its size on the card, against what the writer wrote.
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
  for (uint32_t i = 0; i < sf::kSecs; ++i) r.rows[i] = p.rowsOf(static_cast<sf::Sec>(i));
  // 6. replace(): false keeps the tmp, whole, held for the next save; RAM
  // takes the plan, the card's MARK the one before until it is promoted.
  held_.present = true;
  held_.generation = p.generation;
  held_.headerCrc = headerCrc;
  held_.baseGen = p.head.baseGen;
  if (!tagstore::replace(fs_, kStatsNames)) {
    fold.held(p);
    r.what = Saved::Held;
    r.step = "replace";
    return r;
  }
  // 7. The card's copy is this one: RAM takes the plan, and an old base goes.
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
