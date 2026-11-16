// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the listening stats' files (lib/core StatsFile;
// docs/LISTENING-STATS.md 5.2-5.5, 5.7, 5.12, and S1's rework, 5.14: the
// streaming read and save over statsfold::Fold's records), on a card in
// memory that can lose its power at any step (test/support/CutFs.h),
// against tools/statsref.py's fixtures (test/fixtures/card/stats/index.json
// and sessions.json):
// - the names, and a base's frame as ContainerWriter lays it out; the work
//   memory;
// - StatsCheck: Whole for a whole frame and for Major or UnknownRequired,
//   Unknown for a read the card fails, damaged records or no memory, Torn
//   only for a frame that fails its own checks; a lone major-2 stats.tmp
//   survives settle() (promoted, then read as Newer and left alone);
// - every boot of every fixture: the read's verdict and Why, the boot's
//   fold into the records (and the overflow's chunks), the streaming save,
//   a compaction over three streams, the trim by the card's MARK, and the
//   card after it file for file, byte for byte (stats.bin, the bases,
//   stats.bad, the neighbours removed, the log files trimmed);
// - every session of sessions.json a store can run alone (its saves at
//   statsref's moments): the card byte for byte after each step, each
//   save's outcome, the records and behind; Held at a records' save and at
//   a compacting boot's save, then the save that promotes it, against the
//   same session where that save landed once;
// - the boot's reads again through the wait hook (100, 300, 1,000 ms) and
//   a remount's one; a corrupt file read twice, then stats.bad (an older one
//   removed first; never onto a chain a twin holds); damaged records read
//   once; MARK's block (carved at the boot for a card past 8 rows, never at
//   a remount: no memory);
// - the base: lost only when the card says it isn't there or its header's
//   CRC is another's; an open or a read the card fails, a base shorter than
//   its header, rows out of order or of another count: Unread, nothing
//   removed;
// - the commit: nothing new writes nothing; a held tmp promoted first (its
//   header checked), the generation after it; lost or no longer ours:
//   behind; the card's copy checked first (a change refuses every later
//   save); a full card, a size that doesn't match; a cut at every step of a
//   save (with and without a compaction, three ways), then a boot: the card
//   as the uncut boot left it, no chain freed twice;
// - each cut at its edge (HOUR 840 and 841 rows, DAYS 400 and 401, RCNT's
//   4,096 with a day across it, its 32 days with the clock unset and a day
//   ahead), the compaction's due edge on the merged count (1,365 rows not
//   due, 1,366 due, with records whose hashes have rows already), byte for
//   byte against a reference merge and statsref's retention;
// - a read failing in either pass (the save failed, tried again: the tmp
//   gone, the card's file untouched), a section's CRC failing in either
//   pass (damaged: behind, the file as it was);
// - compaction: base ⊕ TRKD ⊕ the records, a failed one (the save goes on),
//   never over the base the card's stats.bin names;
// - the writer in the caller's memory: no carve during a save, writes in
//   pieces that end on the file's 4 KB boundaries, two files open at once
//   (three for a compaction).
// Run: pio test -e native -f test_stats_file
#include <unity.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../support/CardFixtures.h"
#include "../support/CutFs.h"
#include "CardContainer.h"
#include "CardManifest.h"
#include "PlayLog.h"
#include "StatsFile.h"
#include "StatsFold.h"

namespace cc = cardcontract;
namespace pl = playlog;
namespace sf = statsfold;
namespace sx = statsfile;

void setUp() {}
void tearDown() {}

namespace {

minijson::Value gIndex;
minijson::Value gSessions;
using Bytes = std::string;
using Files = std::map<std::string, Bytes>;
using Rows = std::vector<Bytes>;  // a section's rows, each its stride, in order
const std::string kDir = "/.player/";

// Made-up devices (RFC 7042's documentation MACs) and epochs.
const uint8_t kDevA[6] = {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01};
constexpr uint64_t kEpochA = 0x1122334455667788ull;

const uint8_t* u8(const Bytes& b) { return reinterpret_cast<const uint8_t*>(b.data()); }
uint8_t* u8(Bytes& b) { return reinterpret_cast<uint8_t*>(&b[0]); }

// The caller's memory, aligned as malloc's.
struct Work {
  std::unique_ptr<uint64_t[]> m{new uint64_t[(sx::kWorkBytes + 7) / 8]};
  uint8_t* p() { return reinterpret_cast<uint8_t*>(m.get()); }
};

// The boot's waits, as the hook got them; `gOnWait` runs at each (a card
// that answers differently after a wait).
std::vector<uint32_t> gWaits;
void (*gOnWait)() = nullptr;
void waitHook(uint32_t ms) {
  gWaits.push_back(ms);
  if (gOnWait) gOnWait();
}

// A heap that counts the carves (the Fold's chunks, MARK's block), and a
// carve that fails on demand.
struct Heap {
  std::map<void*, size_t> blocks;
  size_t carves = 0;
  bool fail = false;
};
Heap gHeap;
void* heapAlloc(size_t n) {
  if (gHeap.fail) return nullptr;
  void* p = std::malloc(n);
  gHeap.blocks[p] = n;
  ++gHeap.carves;
  return p;
}
void heapFree(void* p) {
  TEST_ASSERT_TRUE_MESSAGE(gHeap.blocks.count(p) == 1, "a block freed that the heap never gave");
  gHeap.blocks.erase(p);
  std::free(p);
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string hex(const Bytes& a, const Bytes& b) {
  size_t i = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
  char m[96];
  std::snprintf(m, sizeof(m), "%zu and %zu bytes, first difference at %zu", a.size(), b.size(), i);
  return m;
}

// ---------------------------------------------------------------------------
// Files: the frame, a card's /.player.
// ---------------------------------------------------------------------------
struct Sec {
  uint32_t type;
  uint32_t flags;
  uint32_t stride;
  Bytes data;
};

Bytes frameOf(uint32_t gen, const std::vector<Sec>& secs, uint16_t major = 1, uint16_t minor = 0) {
  std::vector<cc::SectionOut> out;
  size_t cap = 40 + 32 * secs.size();
  for (const Sec& s : secs) {
    cc::SectionOut o;
    o.type = s.type;
    o.flags = s.flags;
    o.stride = s.stride;
    o.count = s.stride ? static_cast<uint32_t>(s.data.size() / s.stride) : 0;
    o.bytes = static_cast<uint32_t>(s.data.size());
    out.push_back(o);
    cap += o.bytes + 8;
  }
  std::vector<uint8_t> buf(cap);
  cc::BufferSink sink(buf.data(), static_cast<uint32_t>(buf.size()));
  cc::FileMeta meta;
  meta.magic = sx::kMagic;
  meta.major = major;
  meta.minor = minor;
  meta.generation = gen;
  cc::ContainerWriter w;
  TEST_ASSERT_TRUE(w.begin(sink, meta, nullptr, 40, out.data(), static_cast<uint32_t>(out.size())));
  for (const Sec& s : secs)
    if (!s.data.empty()) TEST_ASSERT_TRUE(w.write(s.data.data(), static_cast<uint32_t>(s.data.size())));
  uint32_t fileBytes = 0;
  TEST_ASSERT_TRUE(w.finish(&fileBytes));
  return Bytes(reinterpret_cast<const char*>(buf.data()), fileBytes);
}

// A stats.bin's (or a base's) sections as they are, through its frame.
std::vector<Sec> sectionsOf(const Bytes& b, const cc::FormatSpec& spec) {
  cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
  cc::Container c;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(c.open(src, spec, nullptr)));
  std::vector<Sec> out;
  for (uint32_t i = 0; i < spec.count; ++i) {
    const cc::Section& s = c.section(i);
    out.push_back({s.type, s.flags, s.stride, b.substr(s.offset, s.bytes)});
  }
  return out;
}

// A section's offset in a stats.bin (for a byte damaged under the session).
uint32_t offsetOf(const Bytes& b, sf::Sec sec) {
  cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
  cc::Container c;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(c.open(src, sx::kStatsSpec, nullptr)));
  return c.section(sec).offset;
}

uint32_t generationOf(const Bytes& b) { return b.size() >= 24 ? cc::get32(u8(b) + 20) : 0; }
uint32_t headerCrcOf(const Bytes& b) { return b.size() >= 36 ? cc::get32(u8(b) + 32) : 0; }

std::string baseName(uint32_t gen) {
  char n[sx::kNameCap];
  TEST_ASSERT_TRUE(sx::baseName(gen, n, sizeof(n)));
  return n;
}

void put(cutfs::CutFs& card, const Files& files) {
  for (const auto& f : files) card.put(kDir + f.first, std::vector<uint8_t>(f.second.begin(), f.second.end()));
}

Bytes bytesOf(const cutfs::CutFs& card, const std::string& name) {
  const std::vector<uint8_t> v = card.bytes(kDir + name);
  return Bytes(v.begin(), v.end());
}

Files filesOf(const cutfs::CutFs& card) {
  Files out;
  for (const std::string& n : card.names())
    if (n.compare(0, kDir.size(), kDir) == 0) out[n.substr(kDir.size())] = bytesOf(card, n.substr(kDir.size()));
  return out;
}

Files cardOf(const minijson::Value& where) {
  Files out;
  for (const auto& m : where.members) out[m.first] = cardfixtures::bytes("stats/" + m.second.str() + "/" + m.first);
  return out;
}

Files sessionCard(const minijson::Value& where) {
  Files out;
  for (const auto& m : where.members)
    out[m.first] = cardfixtures::bytes("stats/sessions/" + m.second.str() + "/" + m.first);
  return out;
}

const minijson::Value& scenario(const char* name) {
  for (size_t i = 0; i < gIndex.size(); ++i)
    if (gIndex[i]["name"].str() == name) return gIndex[i];
  TEST_FAIL_MESSAGE(name);
  return gIndex[0];
}

const minijson::Value& session(const std::string& name) {
  for (size_t i = 0; i < gSessions.size(); ++i)
    if (gSessions[i]["name"].str() == name) return gSessions[i];
  TEST_FAIL_MESSAGE(name.c_str());
  return gSessions[0];
}

void sameFiles(const Files& want, const Files& got, const std::string& what) {
  std::string names;
  for (const auto& f : want)
    if (!got.count(f.first)) names += " -" + f.first;
  for (const auto& f : got)
    if (!want.count(f.first)) names += " +" + f.first;
  TEST_ASSERT_TRUE_MESSAGE(names.empty(), (what + ": the card's names differ:" + names).c_str());
  for (const auto& f : want) {
    const Bytes& g = got.at(f.first);
    TEST_ASSERT_TRUE_MESSAGE(g == f.second, (what + ": " + f.first + ", " + hex(g, f.second)).c_str());
  }
}

// ---------------------------------------------------------------------------
// The card through a hand that can fail one kind of step, that flips one
// byte of one read (a section damaged under the session, seen by that read
// alone), and that records the writes.
// ---------------------------------------------------------------------------
class Wrap : public tagstore::Fs {
public:
  explicit Wrap(cutfs::CutFs& c) : c_(c) {}
  std::string failCreate;   // an open to create a path with this prefix fails
  int failTmpRenames = 0;   // so many renames of stats.tmp to stats.bin fail
  int failBinRemoves = 0;   // so many removes of stats.bin fail
  int sizeOff = 0;          // stats.tmp's size, as a read's open has it, off by this
  std::string flipPath;     // its reads numbered from 0 in `reads`; read `flipRead` has byte `flipAt` flipped
  uint32_t flipAt = 0;
  long flipRead = -1;
  long reads = 0;
  bool flipped = false;     // the flip happened
  struct Write {
    std::string path;
    uint32_t offset, n;
  };
  std::vector<Write> writes;
  std::vector<std::string> creates;  // the opens to create, in order

  class F : public tagstore::File {
  public:
    F(Wrap* w, tagstore::File* in, const std::string& path, int off) : w_(w), in_(in), path_(path), off_(off) {}
    uint32_t size() const override { return static_cast<uint32_t>(static_cast<int64_t>(in_->size()) + off_); }
    bool read(uint32_t offset, void* out, uint32_t n) override {
      if (!in_->read(offset, out, n)) return false;
      if (path_ == w_->flipPath) {
        const long k = w_->reads++;
        if (k == w_->flipRead && offset <= w_->flipAt && w_->flipAt < offset + n) {
          static_cast<uint8_t*>(out)[w_->flipAt - offset] ^= 0xFF;
          w_->flipped = true;
        }
      }
      return true;
    }
    bool write(uint32_t offset, const void* data, uint32_t n) override {
      w_->writes.push_back({path_, offset, n});
      return in_->write(offset, data, n);
    }
    bool sync() override { return in_->sync(); }
    bool truncate(uint32_t size) override { return in_->truncate(size); }
    bool damaged() const override { return in_->damaged(); }
    tagstore::File* in() { return in_; }

  private:
    Wrap* w_;
    tagstore::File* in_;
    std::string path_;
    int off_;
  };

  tagstore::File* open(const char* path, Mode mode) override {
    if (mode == Mode::Create && !failCreate.empty() && std::string(path).compare(0, failCreate.size(), failCreate) == 0)
      return nullptr;
    if (mode == Mode::Create) creates.push_back(path);
    return wrap(c_.open(path, mode), path, 0);
  }
  tagstore::File* openRead(const char* path, bool* cardFailed) override {
    return wrap(c_.openRead(path, cardFailed), path, std::string(path) == sx::kStatsTmp ? sizeOff : 0);
  }
  Entry entry(const char* path) override { return c_.entry(path); }
  bool close(tagstore::File* f) override {
    F* w = static_cast<F*>(f);
    const bool ok = c_.close(w->in());
    delete w;
    return ok;
  }
  bool exists(const char* path) override { return c_.exists(path); }
  bool remove(const char* path) override {
    if (failBinRemoves > 0 && std::string(path) == sx::kStatsBin) {
      --failBinRemoves;
      return false;
    }
    return c_.remove(path);
  }
  bool rename(const char* from, const char* to) override {
    if (failTmpRenames > 0 && std::string(from) == sx::kStatsTmp && std::string(to) == sx::kStatsBin) {
      --failTmpRenames;
      return false;
    }
    return c_.rename(from, to);
  }
  uint32_t firstCluster(const char* path) override { return c_.firstCluster(path); }

private:
  tagstore::File* wrap(tagstore::File* f, const char* path, int off) { return f ? new F(this, f, path, off) : nullptr; }
  cutfs::CutFs& c_;
};

// ---------------------------------------------------------------------------
// A boot as the glue will run it: the read, the boot's fold over the card's
// log in the scan's order (the ranges' block, reserve() before each piece
// of 128), the boot's save, the trim by the card's MARK.
// ---------------------------------------------------------------------------
cc::msmf::Acks acksOf(const minijson::Value& v) {
  cc::msmf::Acks a;
  a.present = v.size() > 0;
  for (size_t i = 0; i < v.size() && a.count < cc::msmf::Acks::kMax; ++i) {
    cc::msmf::Ack& e = a.entries[a.count++];
    const std::string& d = v[i][0].str();
    for (int k = 0; k < 6; ++k) e.deviceId[k] = static_cast<uint8_t>(std::strtoul(d.substr(2 * k, 2).c_str(), nullptr, 16));
    e.epoch = v[i][1].u64();
    e.seq = static_cast<uint32_t>(v[i][2].u64());
  }
  a.rows = a.count;
  return a;
}

std::map<std::string, int> unreadOf(const minijson::Value& b) {
  std::map<std::string, int> u;
  for (const auto& m : b["unread"].members) u[m.first] = m.second.isNull() ? -1 : static_cast<int>(m.second.u64());
  return u;
}

sx::SaveAsk askOf(const minijson::Value& b) {
  sx::SaveAsk a;
  a.nowMs = b["now"].isNull() ? 0 : b["now"].u64();
  a.hasCommit = !b["commitTime"].isNull();
  a.commitSec = b["commitTime"].u64();
  a.mayCompact = true;
  a.playing = b["playing"].boolean();
  a.safePoint = b["safePoint"].boolean();
  return a;
}

const char* const kLogNames[sf::kFiles] = {"plays.jnl",   "plays.1.jnl", "plays.2.jnl", "plays.3.jnl", "plays.4.jnl",
                                           "plays.5.jnl", "plays.6.jnl", "plays.7.jnl", "plays.8.jnl", "plays.9.jnl"};
bool isLog(const std::string& n) {
  for (const char* l : kLogNames)
    if (n == l) return true;
  return false;
}

// The caller's ranges' block for one boot's fold (the glue carves it at the
// top; 8-byte aligned): kept until the fold's end.
struct Ranges {
  std::vector<uint64_t> mem;
  bool begin(sf::Fold& f, const cc::msmf::Acks& acks) {
    const size_t bytes = f.bootBytes(acks);
    mem.assign((bytes + 7) / 8 + 1, 0xA5A5A5A5A5A5A5A5ull);
    return f.beginBoot(acks, mem.data(), bytes);
  }
};

void scanLog(sf::Fold& f, const cutfs::CutFs& card, const std::map<std::string, int>& unread) {
  uint32_t piece = 0;
  for (int i = 0; i < sf::kFiles; ++i) {
    if (!card.dir.count(kDir + kLogNames[i])) continue;
    const Bytes b = bytesOf(card, kLogNames[i]);
    uint32_t slots = static_cast<uint32_t>(std::min<size_t>(b.size(), pl::kFileBytes) / pl::kSlot);
    auto u = unread.find(kLogNames[i]);
    if (u != unread.end()) slots = u->second < 0 ? 0 : std::min<uint32_t>(slots, static_cast<uint32_t>(u->second));
    for (uint32_t k = 0; k < slots; ++k) {
      if (piece++ % 128 == 0) TEST_ASSERT_TRUE(f.reserve(128));
      pl::Slot s;
      if (pl::decode(u8(b) + k * pl::kSlot, pl::kSlot, &s) == pl::Verdict::Ok) f.bootSlot(s, i);
    }
    if (u != unread.end()) f.partial();
  }
}

// 2.20.2's trim as the journal judges a file, and 5.5's: every play slot
// at or under the card's MARK (no fold: a 0.14.0 boot's, today's alone).
bool trimmable(const sf::Fold* f, const Bytes& b, const cc::msmf::Acks& acks) {
  if (b.size() > pl::kFileBytes) return false;
  bool anyPlay = false, allAcked = true, lastIsPlay = false, covered = true;
  for (size_t k = 0; k + pl::kSlot <= b.size(); k += pl::kSlot) {
    pl::Slot s;
    const pl::Verdict v = pl::decode(u8(b) + k, pl::kSlot, &s);
    if (v == pl::Verdict::Newer || v == pl::Verdict::Kind) return false;
    if (v != pl::Verdict::Ok) continue;
    lastIsPlay = s.kind == pl::kKindPlay;
    if (s.kind != pl::kKindPlay) continue;
    anyPlay = true;
    if (!pl::acknowledged(s, &acks)) allAcked = false;
    if (f && !f->covered(s)) covered = false;
  }
  return anyPlay && allAcked && lastIsPlay && covered;
}

// The trim: plays.1.jnl to plays.9.jnl, then plays.jnl.
std::set<std::string> trim(cutfs::CutFs& card, const sf::Fold* f, const cc::msmf::Acks& acks,
                           const std::map<std::string, int>& unread) {
  std::set<std::string> out;
  if (card.dead || acks.count == 0) return out;
  for (int k = 1; k <= sf::kFiles; ++k) {
    const char* n = kLogNames[k % sf::kFiles];
    if (!card.dir.count(kDir + n) || unread.count(n)) continue;
    if (trimmable(f, bytesOf(card, n), acks) && card.remove((kDir + n).c_str())) out.insert(n);
  }
  return out;
}

struct BootOut {
  sx::Opened o;
  sx::SaveReport s;
  std::set<std::string> trimmed;
  uint32_t records = 0;  // the boot's fold's, at its save
};

BootOut bootOnce(cutfs::CutFs& card, sx::Store& store, sf::Fold& f, uint8_t* work, const cc::msmf::Acks& acks,
                 const std::map<std::string, int>& unread, const sx::SaveAsk& ask, bool saveFails = false,
                 sx::Store::WaitFn wait = waitHook) {
  BootOut out;
  out.o = store.read(f, work, wait);
  if (!sx::statsOn(out.o.read)) return out;
  Ranges rg;
  TEST_ASSERT_TRUE(rg.begin(f, acks));
  scanLog(f, card, unread);
  f.endBoot();
  out.records = f.records();
  card.refuse = saveFails;
  out.s = store.save(f, ask, work);
  card.refuse = false;
  out.trimmed = trim(card, &f, acks, unread);
  return out;
}

// One boot of a fixture on `card` (its "in" put there by the caller).
BootOut fixtureBoot(cutfs::CutFs& card, tagstore::Fs& fs, const minijson::Value& b, Work& w,
                    sx::Store::WaitFn wait = waitHook) {
  sf::Fold f;
  sx::Store store(fs);
  return bootOnce(card, store, f, w.p(), acksOf(b["acks"]), unreadOf(b), askOf(b), b["saveFails"].boolean(), wait);
}

// stats.bin's bytes with another major (its header's CRC made again).
Bytes resealed(Bytes b, uint16_t major) {
  uint8_t* p = u8(b);
  cc::put16(p + 4, major);
  const uint32_t hb = cc::get32(p + 8), n = cc::get32(p + 12);
  cc::put32(p + 32, 0);
  cc::put32(p + 32, cc::crc32(p, hb + 32 * n));
  return b;
}

// ---------------------------------------------------------------------------
// The save in miniature (test_stats_fold's): each section's card rows merged
// with the records' (the cursor), statsref's retain() after, a compaction
// on the merged TRKD's count, HEAD and MARK from the plan. The reference the
// streaming save's bytes are held to where no fixture has the rows.
// ---------------------------------------------------------------------------
Rows rowsOf(const Bytes& data, uint32_t stride, sf::Sec sec) {
  Rows out;
  for (size_t i = 0; i + stride <= data.size(); i += stride) out.push_back(data.substr(i, sf::kStrides[sec]));
  return out;
}

Rows merged(sf::Fold& f, sf::Sec sec, const Rows& card) {
  const uint32_t st = sf::kStrides[sec];
  sf::Fold::Cursor c(f, sec);
  Rows out;
  uint8_t r[sf::kTrackBytes];
  bool more = c.next(r);
  size_t i = 0;
  while (more || i < card.size()) {
    const int k = !more ? 1 : i == card.size() ? -1 : sf::compareRows(sec, r, u8(card[i]));
    if (k < 0) {
      out.push_back(Bytes(reinterpret_cast<const char*>(r), st));
    } else if (k > 0) {
      out.push_back(card[i++]);
    } else {
      Bytes a = card[i++];
      sf::addRow(sec, u8(a), r);
      out.push_back(a);
    }
    if (k <= 0) more = c.next(r);
  }
  return out;
}

uint16_t dayOfRow(const Bytes& r) { return cc::get16(u8(r)); }

// statsref's retain(): HOUR's and DAYS' newest 840 and 400; RCNT's 32 days
// back from the earlier of today and DAYS' newest day (nothing by date with
// the clock unset), then whole days, oldest first, past 4,096 rows.
void retain(Rows rows[sf::kSecs], uint64_t nowMs) {
  Rows& hr = rows[sf::kHour];
  Rows& dy = rows[sf::kDays];
  Rows& rc = rows[sf::kRcnt];
  if (hr.size() > sf::kHourRows) hr.erase(hr.begin(), hr.end() - sf::kHourRows);
  if (dy.size() > sf::kDaysRows) dy.erase(dy.begin(), dy.end() - sf::kDaysRows);
  uint16_t today = 0;
  if (sf::todayOf(nowMs, &today) && !dy.empty()) {
    const int64_t ref = std::min<int64_t>(today, dayOfRow(dy.back()));
    rc.erase(std::remove_if(rc.begin(), rc.end(),
                            [&](const Bytes& r) { return dayOfRow(r) <= ref - static_cast<int64_t>(sf::kRcntDays); }),
             rc.end());
  }
  while (rc.size() > sf::kRcntRows) {
    const uint16_t oldest = dayOfRow(rc.front());
    size_t k = 0;
    while (k < rc.size() && dayOfRow(rc[k]) == oldest) ++k;
    rc.erase(rc.begin(), rc.begin() + k);
  }
}

Bytes join(const Rows& rows) {
  Bytes out;
  for (const Bytes& r : rows) out += r;
  return out;
}

struct Expect {
  Bytes bin, base;
  bool compacted = false;
  uint32_t rows[sf::kSecs] = {};
};

// The save's bytes, the fold unchanged (plan() changes no RAM). `bin`: the
// card's stats.bin ("": none); `base`: the base it names ("": none); `named`:
// the base the card's stats.bin names (no compaction under it).
Expect expectSave(sf::Fold& f, const Bytes& bin, const Bytes& base, uint32_t named, const sx::SaveAsk& ask) {
  Expect o;
  sf::Fold::Plan p;
  TEST_ASSERT_TRUE(f.plan(ask.nowMs, ask.hasCommit, ask.commitSec, &p));
  Rows m[sf::kSecs];
  std::vector<Sec> secs;
  if (!bin.empty()) secs = sectionsOf(bin, sx::kStatsSpec);
  for (int i = sf::kHour; i < sf::kSecs; ++i) {
    const sf::Sec sec = static_cast<sf::Sec>(i);
    m[i] = merged(f, sec, secs.empty() ? Rows() : rowsOf(secs[i].data, secs[i].stride, sec));
  }
  retain(m, ask.nowMs);
  if (ask.mayCompact && sf::compactionDue(static_cast<uint32_t>(m[sf::kTrkd].size()), p.head.baseRows, ask.playing,
                                          ask.safePoint) &&
      p.head.baseGen + 1 != named) {
    Rows b;
    if (!base.empty()) {
      const std::vector<Sec> t = sectionsOf(base, sx::kBaseSpec);
      b = rowsOf(t[0].data, t[0].stride, sf::kTrkd);
    }
    Rows rows;
    size_t i = 0, k = 0;
    const Rows& d = m[sf::kTrkd];
    while (i < b.size() || k < d.size()) {
      const int c = i == b.size() ? 1 : k == d.size() ? -1 : sf::compareRows(sf::kTrkd, u8(b[i]), u8(d[k]));
      if (c < 0) {
        rows.push_back(b[i++]);
      } else if (c > 0) {
        rows.push_back(d[k++]);
      } else {
        Bytes a = b[i++];
        sf::addRow(sf::kTrkd, u8(a), u8(d[k++]));
        rows.push_back(a);
      }
    }
    const uint32_t gen = p.head.baseGen + 1;
    o.base = frameOf(gen, {{sf::kTrks, cc::kSectionRequired, 24, join(rows)}});
    p.head.baseGen = gen;
    p.head.baseRows = static_cast<uint32_t>(rows.size());
    p.head.baseCrc = headerCrcOf(o.base);
    o.compacted = true;
    m[sf::kTrkd].clear();
  }
  Bytes head(sf::kHeadBytes, '\0'), mark(static_cast<size_t>(p.rows[sf::kMark]) * 24, '\0');
  sf::encodeHead(p.head, u8(head));
  TEST_ASSERT_EQUAL_UINT32(p.rows[sf::kMark], f.markRows(0, u8(mark), p.rows[sf::kMark]));
  o.bin = frameOf(p.generation, {{sf::kTypes[0], cc::kSectionRequired, 0, head},
                                 {sf::kTypes[1], cc::kSectionRequired, 24, mark},
                                 {sf::kTypes[2], cc::kSectionRequired, 8, join(m[sf::kHour])},
                                 {sf::kTypes[3], cc::kSectionRequired, 16, join(m[sf::kDays])},
                                 {sf::kTypes[4], cc::kSectionRequired, 12, join(m[sf::kRcnt])},
                                 {sf::kTypes[5], cc::kSectionRequired, 24, join(m[sf::kTrkd])}});
  for (int i = sf::kHour; i < sf::kSecs; ++i) o.rows[i] = static_cast<uint32_t>(m[i].size());
  o.rows[sf::kHead] = 1;
  o.rows[sf::kMark] = p.rows[sf::kMark];
  return o;
}

// A stats.bin made up: HEAD (`h`), MARK's rows, then HOUR, DAYS, RCNT and
// TRKD's row bytes.
Bytes statsFile(uint32_t gen, const sf::Head& h, const Bytes& mark, const Bytes& hour, const Bytes& days,
                const Bytes& rcnt, const Bytes& trkd) {
  Bytes head(sf::kHeadBytes, '\0');
  sf::encodeHead(h, u8(head));
  return frameOf(gen, {{sf::kTypes[0], cc::kSectionRequired, 0, head},
                       {sf::kTypes[1], cc::kSectionRequired, 24, mark},
                       {sf::kTypes[2], cc::kSectionRequired, 8, hour},
                       {sf::kTypes[3], cc::kSectionRequired, 16, days},
                       {sf::kTypes[4], cc::kSectionRequired, 12, rcnt},
                       {sf::kTypes[5], cc::kSectionRequired, 24, trkd}});
}

Bytes markRow(const uint8_t dev[6], uint64_t epoch, uint32_t seq) {
  Bytes r(24, '\0');
  std::memcpy(u8(r), dev, 6);
  cc::put64(u8(r) + 8, epoch);
  cc::put32(u8(r) + 16, seq);
  return r;
}
Bytes hourRow(uint32_t hour, uint8_t plays, uint32_t ms) {
  Bytes r(8, '\0');
  cc::put32(u8(r), hour | static_cast<uint32_t>(plays) << 24);
  cc::put32(u8(r) + 4, ms);
  return r;
}
Bytes daysRow(uint16_t day, uint16_t plays, uint32_t ms) {
  Bytes r(16, '\0');
  cc::put16(u8(r), day);
  cc::put16(u8(r) + 2, plays);
  cc::put32(u8(r) + 8, ms);
  return r;
}
Bytes rcntRow(uint16_t day, uint16_t plays, uint64_t hash) {
  Bytes r(12, '\0');
  cc::put16(u8(r), day);
  cc::put16(u8(r) + 2, plays);
  cc::put64(u8(r) + 4, hash);
  return r;
}
Bytes trackRow(uint64_t hash, uint16_t plays, uint32_t at, uint32_t ord) {
  sf::Track t;
  t.hash = hash;
  t.plays = plays;
  t.firstAt = t.lastAt = at;
  t.lastOrd = ord;
  Bytes r(24, '\0');
  sf::encodeTrack(t, u8(r));
  return r;
}

uint64_t hashOf(uint32_t i) { return cc::fnv1a64(&i, sizeof(i)); }

uint64_t at(uint32_t day, uint32_t hour, uint32_t minute = 0) {
  return sf::kMinMs + static_cast<uint64_t>(day) * sf::kDayMs + static_cast<uint64_t>(hour) * sf::kHourMs +
         static_cast<uint64_t>(minute) * 60000;
}

// A play of A's: seq q, a track's hash, dated at `startedAt` (0: undated).
pl::Slot playA(uint32_t q, uint64_t hash, uint64_t startedAt) {
  pl::Slot s;
  s.kind = pl::kKindPlay;
  s.outcome = pl::kCompleted;
  s.source = pl::kSourceManual;
  s.clock = startedAt ? pl::kClockRtc : pl::kClockUnset;
  s.seq = q;
  s.epoch = kEpochA;
  std::memcpy(s.deviceId, kDevA, 6);
  s.startedAt = startedAt;
  s.bootSeq = 1;
  s.uptimeMs = q * 1000;
  s.playedMs = 200000;
  s.durationMs = 200000;
  s.pathHash = hash;
  return s;
}

// A card whose stats.bin is `bin` (and base 1 `base`, when given), read
// into `f` through `store` (the boot's), its records then `slots` folded live.
void loadAndFold(cutfs::CutFs& card, sx::Store& store, sf::Fold& f, Work& w, const Bytes& bin, const Bytes& base,
                 const std::vector<pl::Slot>& slots) {
  put(card, {{"stats.bin", bin}});
  if (!base.empty()) put(card, {{"stats-00000001.bin", base}});
  TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(store.read(f, w.p(), waitHook).read));
  for (const pl::Slot& s : slots) TEST_ASSERT_TRUE(f.liveSlot(s));
}

}  // namespace

// ---------------------------------------------------------------------------
// The names and the frame.
// ---------------------------------------------------------------------------
void test_the_names_and_the_frame() {
  TEST_ASSERT_EQUAL_STRING("/.player/stats.bin", sx::kStatsNames.path);
  TEST_ASSERT_EQUAL_STRING("/.player/stats.tmp", sx::kStatsNames.tmp);
  TEST_ASSERT_EQUAL_STRING("/.player/stats", sx::kStatsNames.stem);
  char t[sx::kNameCap];
  TEST_ASSERT_TRUE(tagstore::twinName(sx::kStatsNames, 9, t, sizeof(t)));
  TEST_ASSERT_EQUAL_STRING("/.player/stats.xl9", t);
  TEST_ASSERT_EQUAL_STRING("/.player/stats-0000002a.bin", baseName(0x2a).c_str());
  TEST_ASSERT_EQUAL_STRING("/.player/stats-ffffffff.bin", baseName(0xFFFFFFFFu).c_str());
  char small[8];
  TEST_ASSERT_FALSE(sx::baseName(1, small, sizeof(small)));
  // A base's frame by hand is ContainerWriter's, for 0, 1 and 5 rows.
  for (uint32_t rows : {0u, 1u, 5u}) {
    Bytes data;
    for (uint32_t i = 0; i < rows; ++i) {
      sf::Track tr;
      tr.hash = 1000 + 7 * i;
      tr.plays = static_cast<uint16_t>(i + 1);
      tr.firstAt = tr.lastAt = 26948401 + i;
      tr.lastOrd = 3;
      tr.src = static_cast<uint8_t>(i % 3);
      uint8_t r[24];
      sf::encodeTrack(tr, r);
      data.append(reinterpret_cast<const char*>(r), 24);
    }
    const Bytes want = frameOf(0x2a, {{sf::kTrks, cc::kSectionRequired, 24, data}});
    uint8_t head[sx::kBaseRowsAt];
    uint32_t crc = 0;
    sx::baseFrame(0x2a, rows, cc::crc32(data.data(), data.size()), head, &crc);
    const Bytes mine = Bytes(reinterpret_cast<const char*>(head), sizeof(head)) + data;
    TEST_ASSERT_TRUE_MESSAGE(mine == want, hex(mine, want).c_str());
    TEST_ASSERT_EQUAL_HEX32(headerCrcOf(want), crc);
  }
  // The fixtures' bases are such frames.
  const Bytes base = cardOf(scenario("worked-example")["boots"][0]["in"]).at("stats-00000001.bin");
  const std::vector<Sec> s = sectionsOf(base, sx::kBaseSpec);
  uint8_t head[sx::kBaseRowsAt];
  sx::baseFrame(1, static_cast<uint32_t>(s[0].data.size() / 24), cc::crc32(s[0].data.data(), s[0].data.size()), head,
                nullptr);
  TEST_ASSERT_EQUAL_MEMORY(base.data(), head, sizeof(head));
  // The caller's memory: the scratch (the writer, the plan, two frames, two
  // row readers, a cursor), then 4 KB for the writer and two 2 KB reads.
  TEST_ASSERT_TRUE(sizeof(sx::Scratch) <= sx::kScratchBytes);
  TEST_ASSERT_EQUAL_UINT32(4096, sx::kBufBytes);
  TEST_ASSERT_EQUAL_UINT32(2048, sx::kReadBytes);
  TEST_ASSERT_EQUAL_UINT32(sx::kScratchBytes + 8192, sx::kWorkBytes);
  printf("the work memory: %u bytes (the scratch %u: the writer %u, the rest %u)\n",
         static_cast<unsigned>(sx::kWorkBytes), static_cast<unsigned>(sx::kScratchBytes),
         static_cast<unsigned>(sizeof(cc::ContainerWriter)),
         static_cast<unsigned>(sizeof(sx::Scratch) - sizeof(cc::ContainerWriter)));
}

// ---------------------------------------------------------------------------
// StatsCheck.
// ---------------------------------------------------------------------------
void test_stats_check() {
  const Bytes good = cardOf(scenario("worked-example")["boots"][0]["out"]).at("stats.bin");
  std::vector<uint8_t> buf(sx::kBufBytes);
  sx::StatsCheck check(buf.data(), static_cast<uint32_t>(buf.size()));
  using V = tagstore::TmpCheck::Verdict;
  auto verdict = [&](const Bytes& b) {
    cutfs::CutFs card;
    card.put(sx::kStatsTmp, std::vector<uint8_t>(b.begin(), b.end()));
    return check.check(card, sx::kStatsTmp);
  };
  TEST_ASSERT_TRUE(verdict(good) == V::Whole);
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(check.why()));
  // A newer stats firmware's: whole to this one.
  TEST_ASSERT_TRUE(verdict(resealed(good, 2)) == V::Whole);
  TEST_ASSERT_EQUAL_STRING("major", cc::whyName(check.why()));
  std::vector<Sec> secs = sectionsOf(good, sx::kStatsSpec);
  secs.push_back({cc::fourcc("XTRA"), cc::kSectionRequired, 4, Bytes(8, '\0')});
  TEST_ASSERT_TRUE(verdict(frameOf(generationOf(good), secs)) == V::Whole);
  TEST_ASSERT_EQUAL_STRING("unknownRequired", cc::whyName(check.why()));
  secs.back().flags = 0;  // an OPTIONAL one: a newer minor's, whole
  TEST_ASSERT_TRUE(verdict(frameOf(generationOf(good), secs, 1, 3)) == V::Whole);
  // Torn: a frame that fails its own checks.
  TEST_ASSERT_TRUE(verdict(good.substr(0, good.size() - 1)) == V::Torn);
  TEST_ASSERT_EQUAL_STRING("fileBytes", cc::whyName(check.why()));
  Bytes flipped = good;
  flipped[flipped.size() - 3] ^= 1;
  TEST_ASSERT_TRUE(verdict(flipped) == V::Torn);
  TEST_ASSERT_EQUAL_STRING("sectionCrc", cc::whyName(check.why()));
  TEST_ASSERT_TRUE(verdict(Bytes(39, 'x')) == V::Torn);
  TEST_ASSERT_TRUE(verdict(Bytes(good.size(), '\0')) == V::Torn);  // a cut before its header
  {
    cutfs::CutFs card;
    TEST_ASSERT_TRUE(check.check(card, sx::kStatsTmp) == V::Torn);  // not there to open
  }
  // Unknown: a read the card fails (the open, then a section's read), damaged records, no memory.
  for (long from : {0L, 1L, 4L}) {
    cutfs::CutFs card;
    card.put(sx::kStatsTmp, std::vector<uint8_t>(good.begin(), good.end()));
    card.faultPrefix = sx::kStatsTmp;
    card.faultFrom = from;
    TEST_ASSERT_TRUE(check.check(card, sx::kStatsTmp) == V::Unknown);
    TEST_ASSERT_EQUAL_STRING("io", cc::whyName(check.why()));
  }
  {
    cutfs::CutFs card;
    card.put(sx::kStatsTmp, std::vector<uint8_t>(good.begin(), good.end()));
    card.damagedPrefix = sx::kStatsTmp;
    TEST_ASSERT_TRUE(check.check(card, sx::kStatsTmp) == V::Unknown);
    TEST_ASSERT_EQUAL_STRING("fatRecords", cc::whyName(check.why()));
  }
  sx::StatsCheck none(nullptr, 0);
  cutfs::CutFs card;
  card.put(sx::kStatsTmp, std::vector<uint8_t>(good.begin(), good.end()));
  TEST_ASSERT_TRUE(none.check(card, sx::kStatsTmp) == V::Unknown);
}

void test_a_lone_major_2_tmp_survives_settle() {
  // A cut between replace()'s remove and its rename, by a newer stats
  // firmware: its stats.tmp alone. Promoted, then read as Newer and left
  // alone: no fold, no save, the file as it was.
  const Bytes good = cardOf(scenario("worked-example")["boots"][0]["out"]).at("stats.bin");
  const Bytes newer = resealed(good, 2);
  Work w;
  {
    cutfs::CutFs card;
    put(card, {{"stats.tmp", newer}});
    sf::Fold f;
    sx::Store store(card);
    gWaits.clear();
    const sx::Opened o = store.read(f, w.p(), waitHook);
    TEST_ASSERT_TRUE(o.settled.what == tagstore::Settle::Promoted);
    TEST_ASSERT_EQUAL_STRING("newer", sx::readName(o.read));
    TEST_ASSERT_EQUAL_STRING("major", cc::whyName(o.why));
    TEST_ASSERT_FALSE(store.on());
    sameFiles({{"stats.bin", newer}}, filesOf(card), "a lone major-2 tmp");
    TEST_ASSERT_TRUE(store.save(f, sx::SaveAsk(), w.p()).what == sx::Saved::Off);
    TEST_ASSERT_EQUAL_UINT32(0, card.violations.size());
    TEST_ASSERT_EQUAL_UINT32(0, gWaits.size());
  }
  {
    // Ours (major 1), whole: promoted and loaded. Torn: removed, a fresh start.
    cutfs::CutFs card;
    put(card, {{"stats.tmp", good}});
    sf::Fold f;
    sx::Store store(card);
    sx::Opened o = store.read(f, w.p(), waitHook);
    TEST_ASSERT_TRUE(o.settled.what == tagstore::Settle::Promoted);
    TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o.read));
    TEST_ASSERT_EQUAL_UINT32(generationOf(good), f.generation());
    cutfs::CutFs torn;
    put(torn, {{"stats.tmp", good.substr(0, 100)}});
    sx::Store s2(torn);
    o = s2.read(f, w.p(), waitHook);
    TEST_ASSERT_TRUE(o.settled.what == tagstore::Settle::Removed);
    TEST_ASSERT_EQUAL_STRING("absent", sx::readName(o.read));
    TEST_ASSERT_EQUAL_UINT32(0, filesOf(torn).size());
  }
}

// ---------------------------------------------------------------------------
// Every boot of every fixture.
// ---------------------------------------------------------------------------
void test_every_fixture_boot() {
  Work w;
  int boots = 0, saves = 0, compactions = 0, trims = 0, removed = 0, bad = 0, past = 0;
  uint32_t peakOpen = 0;
  for (size_t i = 0; i < gIndex.size(); ++i) {
    const minijson::Value& sc = gIndex[i];
    for (size_t k = 0; k < sc["boots"].size(); ++k) {
      const minijson::Value& b = sc["boots"][k];
      char what[96];
      std::snprintf(what, sizeof(what), "%s, boot %u", sc["name"].c_str(), static_cast<unsigned>(k + 1));
      cutfs::CutFs card;
      put(card, cardOf(b["in"]));
      gWaits.clear();
      const BootOut r = fixtureBoot(card, card, b, w);
      ++boots;
      TEST_ASSERT_EQUAL_STRING_MESSAGE(b["read"].c_str(), sx::readName(r.o.read), what);
      if (!b["why"].isNull())
        TEST_ASSERT_EQUAL_STRING_MESSAGE(lower(b["why"].str()).c_str(), lower(cc::whyName(r.o.why)).c_str(), what);
      TEST_ASSERT_EQUAL_MESSAGE(b["read"].str() == "corrupt", r.o.quarantined, what);
      bad += r.o.quarantined;
      TEST_ASSERT_EQUAL_MESSAGE(b["saved"].boolean(), r.s.what == sx::Saved::Wrote, what);
      TEST_ASSERT_EQUAL_MESSAGE(b["compacted"].boolean(), r.s.compacted, what);
      saves += r.s.what == sx::Saved::Wrote;
      compactions += r.s.compacted;
      past += r.records > sf::kRecords;
      if (b["saveFails"].boolean()) TEST_ASSERT_EQUAL_STRING_MESSAGE("failed", sx::savedName(r.s.what), what);
      if (b["partial"].boolean()) TEST_ASSERT_EQUAL_STRING_MESSAGE("off", sx::savedName(r.s.what), what);
      // What the read removed (the neighbours) and the compaction's old base.
      const uint32_t gone = r.o.removed + (r.s.oldBaseRemoved ? 1 : 0);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(b["removed"].size(), gone, what);
      removed += static_cast<int>(gone);
      std::set<std::string> want;
      for (size_t t = 0; t < b["trimmed"].size(); ++t) want.insert(b["trimmed"][t].str());
      TEST_ASSERT_TRUE_MESSAGE(want == r.trimmed, what);
      trims += static_cast<int>(r.trimmed.size());
      sameFiles(cardOf(b["out"]), filesOf(card), what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.violations.size(), what);
      // A save opens two files at once, a compaction three.
      TEST_ASSERT_TRUE_MESSAGE(card.peakOpen <= (r.s.compacted || r.s.compactFailed ? 3u : 2u), what);
      peakOpen = std::max(peakOpen, card.peakOpen);
    }
  }
  printf("fixtures: %d boots, %d saves and %d bases byte for byte, %d removed, %d stats.bad, %d files trimmed, "
         "%d boots past the records; at most %u files open\n",
         boots, saves, compactions, removed, bad, trims, past, static_cast<unsigned>(peakOpen));
  TEST_ASSERT_EQUAL_INT(34, boots);
  TEST_ASSERT_EQUAL_INT(25, saves);
  TEST_ASSERT_EQUAL_INT(4, compactions);
  TEST_ASSERT_EQUAL_INT(2, removed);
  TEST_ASSERT_EQUAL_INT(1, bad);
  TEST_ASSERT_EQUAL_INT(22, trims);
  TEST_ASSERT_EQUAL_INT(2, past);
  TEST_ASSERT_EQUAL_UINT32(3, peakOpen);
}

// ---------------------------------------------------------------------------
// Every session a store can run alone: the saves at statsref's moments
// (sessions.json's facts say which save a step asks: the glue's moments,
// R5's), the slots folded live from each play's fields, the log's files as
// the journal left them after each step. The card byte for byte after each
// step, each save's outcome, the records, behind.
// ---------------------------------------------------------------------------
namespace {

std::string outcomeOf(const sx::SaveReport& r, const sf::Fold& f) {
  switch (r.what) {
    case sx::Saved::Wrote: return "wrote";
    case sx::Saved::Nothing: return "nothing";
    case sx::Saved::Held: return "held";
    case sx::Saved::Failed: return "failed";
    case sx::Saved::Changed: return "changed";
    case sx::Saved::Behind:
    case sx::Saved::Damaged: return "behind";
    case sx::Saved::Off: return f.partialScan() ? "partial" : "off";
    case sx::Saved::NoMemory: return "noMemory";
  }
  return "?";
}

// The glue's refusals (the end's flush, its budget, the fence): no store call.
bool glueRefuses(const std::string& o) { return o == "ending" || o == "budget" || o == "fenced"; }

struct SPlayer {
  uint8_t dev[6] = {};
  std::vector<uint64_t> epochs;
  size_t next = 0;
  bool nvs = false;
  uint64_t epoch = 0;
  bool fenced = false;
  std::unique_ptr<sf::Fold> fold;
  std::unique_ptr<sx::Store> store;
};

struct SessionRun {
  cutfs::CutFs card;
  Wrap fs{card};
  Work w;
  std::map<std::string, SPlayer> players;
  int saves = 0, held = 0, behind = 0;
  // A replace() that fails (statsref's replaceFails): at stats.bin's remove,
  // or with no stats.bin at the tmp's rename.
  sx::SaveReport save(SPlayer& p, const minijson::Value& st, bool mayCompact, bool fail, bool replaceFails,
                      const minijson::Value& now) {
    sx::SaveAsk a;
    a.nowMs = now.isNull() ? 0 : now.u64();
    a.hasCommit = !st["commitTime"].isNull();
    a.commitSec = st["commitTime"].u64();
    a.mayCompact = mayCompact;
    a.playing = st["playing"].boolean();
    a.safePoint = st["safePoint"].boolean();
    card.refuse = fail;
    if (replaceFails) fs.failBinRemoves = fs.failTmpRenames = 1;
    const sx::SaveReport r = p.store->save(*p.fold, a, w.p());
    card.refuse = false;
    fs.failBinRemoves = fs.failTmpRenames = 0;
    ++saves;
    held += r.what == sx::Saved::Held;
    behind += r.what == sx::Saved::Behind || r.what == sx::Saved::Damaged;
    return r;
  }
  // A save the facts ask (`want`: statsref's outcome; null: none asked).
  void asked(SPlayer& p, const minijson::Value& st, const minijson::Value& now, const minijson::Value& want,
             bool fail, bool replaceFails, const std::string& what) {
    if (want.isNull() || glueRefuses(want.str())) return;
    std::string got = "off";
    if (p.store) got = outcomeOf(save(p, st, false, fail, replaceFails, now), *p.fold);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), got.c_str(), what.c_str());
  }
  // A play written: the slot as the journal wrote it, from its fields.
  void fold(SPlayer& p, const minijson::Value& st, size_t i, const minijson::Value& seq) {
    if (seq.isNull() || !p.store || !p.store->on()) return;
    auto col = [&](const char* k) -> const minijson::Value& { return i == SIZE_MAX ? st[k] : st["plays"][k][i]; };
    pl::Slot s;
    s.kind = pl::kKindPlay;
    s.outcome = static_cast<uint8_t>(col("outcome").u64());
    s.source = static_cast<uint8_t>(col("src").u64());
    s.startedAt = col("at").u64();
    s.clock = s.startedAt ? pl::kClockRtc : pl::kClockUnset;
    s.seq = static_cast<uint32_t>(seq.u64());
    s.epoch = p.epoch;
    std::memcpy(s.deviceId, p.dev, 6);
    s.uptimeMs = static_cast<uint32_t>(col("up").u64());
    s.playedMs = static_cast<uint32_t>(col("played").u64());
    s.durationMs = static_cast<uint32_t>(col("duration").u64());
    s.pathHash = col("hash").u64();
    s.size = static_cast<uint32_t>(col("size").u64());
    s.fatTime = static_cast<uint32_t>(col("fatTime").u64());
    s.generation = static_cast<uint32_t>(col("generation").u64());
    p.fold->liveSlot(s);
  }
  // The log's files as the journal left them after the step.
  void logs(const Files& out) {
    for (const std::string& n : card.names()) {
      const std::string name = n.substr(kDir.size());
      if (n.compare(0, kDir.size(), kDir) == 0 && isLog(name) && !out.count(name)) card.remove(n.c_str());
    }
    for (const auto& f : out)
      if (isLog(f.first)) put(card, {{f.first, f.second}});
  }
};

// Whether the store can run a session alone: one card, its steps the
// store's (or the glue's refusals, or the player's NVS), no twin's link.
bool storeRuns(const minijson::Value& sc) {
  for (size_t i = 0; i < sc["steps"].size(); ++i)
    if (sc["steps"][i]["do"].str() == "link") return false;
  return true;
}

// The session's card as it starts, and its players (device, epochs, NVS).
void startSession(const minijson::Value& sc, SessionRun& run) {
  put(run.card, sessionCard(sc["in"]));
  for (const auto& m : sc["players"].members) {
    SPlayer& p = run.players[m.first];
    const std::string& d = m.second["device"].str();
    for (int k = 0; k < 6; ++k) p.dev[k] = static_cast<uint8_t>(std::strtoul(d.substr(2 * k, 2).c_str(), nullptr, 16));
    for (size_t e = 0; e < m.second["epochs"].size(); ++e) p.epochs.push_back(m.second["epochs"][e].u64());
    if (!m.second["nvs"].isNull()) {
      p.nvs = true;
      p.epoch = m.second["nvs"][0].u64();
    }
  }
}

void runSession(const minijson::Value& sc, SessionRun& run, int* steps) {
  const std::string name = sc["name"].str();
  startSession(sc, run);
  for (size_t i = 0; i < sc["steps"].size(); ++i) {
    const minijson::Value& st = sc["steps"][i];
    const std::string what = name + ", step " + std::to_string(i + 1) + " (" + st["do"].str() + ")";
    const std::string doing = st["do"].str();
    SPlayer* p = st["player"].isNull() ? nullptr : &run.players.at(st["player"].str());
    const Files out = sessionCard(st["out"]);
    if (doing == "boot") {
      if (!p->nvs) {
        p->epoch = p->epochs.at(p->next++);
        p->nvs = true;
      }
      p->fenced = false;
      const cc::msmf::Acks acks = acksOf(st["acks"]);
      const std::map<std::string, int> unread = unreadOf(st);
      p->store.reset();
      p->fold.reset();
      if (!st["stats"].boolean()) {
        trim(run.card, nullptr, acks, unread);  // a 0.14.0 boot: today's trim
      } else {
        p->fold.reset(new sf::Fold);
        p->store.reset(new sx::Store(run.fs));
        gWaits.clear();
        const sx::Opened o = p->store->read(*p->fold, run.w.p(), waitHook);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(st["read"].c_str(), sx::readName(o.read), what.c_str());
        Ranges rg;
        if (p->store->on()) {
          TEST_ASSERT_TRUE_MESSAGE(rg.begin(*p->fold, acks), what.c_str());
          scanLog(*p->fold, run.card, unread);
        }
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(st["folded"].u64(), p->fold->folded(), what.c_str());
        // The boot's save (none after a partial scan: the fold ends at
        // Ready), then the trim, then an overflow's tries (BOOT_TRIES in all
        // while they fail), behind when none lands.
        const minijson::Value& fails = st["saveFails"];
        const int failing = fails.boolean() ? 4 : static_cast<int>(fails.u64());
        int tries = 0;
        bool wrote = false, compacted = false;
        uint32_t removed = o.removed;
        std::string outcome;
        auto once = [&] {
          ++tries;
          const sx::SaveReport r =
              run.save(*p, st, true, tries <= failing, st["replaceFails"].boolean(), st["now"]);
          wrote = wrote || r.what == sx::Saved::Wrote;
          compacted = compacted || r.compacted;
          removed += r.oldBaseRemoved ? 1 : 0;
          return outcomeOf(r, *p->fold);
        };
        if (!p->fold->partialScan()) {
          outcome = once();
        } else {
          p->fold->endBoot();
        }
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(st["twice"].u64(), p->fold->twice(), what.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(st["gap"].u64(), p->fold->gap(), what.c_str());
        std::set<std::string> trimmed;
        if (p->store->on()) trimmed = trim(run.card, p->fold.get(), acks, unread);
        if (p->store->on() && p->fold->records() > sf::kRecords && outcome != "wrote" && outcome != "held") {
          while (outcome == "failed" && tries < 4) outcome = once();
          if (outcome != "wrote" && outcome != "held") p->fold->fallBehind();
        }
        TEST_ASSERT_EQUAL_STRING_MESSAGE(st["bootSave"].isNull() ? "" : st["bootSave"].c_str(), outcome.c_str(),
                                         what.c_str());
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(st["tries"].u64()), tries, what.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(st["saved"].boolean(), wrote, what.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(st["compacted"].boolean(), compacted, what.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(st["removed"].size(), removed, what.c_str());
        std::set<std::string> want;
        for (size_t t = 0; t < st["trimmed"].size(); ++t) want.insert(st["trimmed"][t].str());
        TEST_ASSERT_TRUE_MESSAGE(want == trimmed, what.c_str());
      }
    } else if (doing == "play") {
      const bool fail = st["saveFails"].boolean(), replace = st["replaceFails"].boolean();
      if (!p->fenced) run.asked(*p, st, st["now"], st["recordsSave"], fail, replace, what + ", the records' save");
      run.asked(*p, st, st["now"], st["rotationSave"], fail, replace, what + ", the rotation's save");
      run.logs(out);
      run.fold(*p, st, SIZE_MAX, st["seq"]);
      if (p->fenced)
        run.asked(*p, st, st["now"], st["recordsSave"], fail, replace, what + ", the records' save at 96");
    } else if (doing == "plays") {
      const minijson::Value& each = st["each"];
      const bool fail = st["saveFails"].boolean();
      for (size_t k = 0; k < each["seq"].size(); ++k) {
        const std::string one = what + ", play " + std::to_string(k + 1);
        const minijson::Value& now = st["plays"]["now"][k];
        if (!p->fenced) run.asked(*p, st, now, each["recordsSave"][k], fail, false, one + ", the records' save");
        run.asked(*p, st, now, each["rotationSave"][k], fail, false, one + ", the rotation's save");
        run.fold(*p, st, k, each["seq"][k]);
        if (p->fenced) run.asked(*p, st, now, each["recordsSave"][k], fail, false, one + ", the records' save at 96");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(each["records"][k].u64(), p->fold ? p->fold->records() : 0, one.c_str());
      }
      run.logs(out);
    } else if (doing == "hs") {
      run.asked(*p, st, st["now"], st["recordsSave"], st["saveFails"].boolean(), st["replaceFails"].boolean(),
                what + ", the records' save");
      run.asked(*p, st, st["now"], st["hsSave"], st["saveFails"].boolean(), st["replaceFails"].boolean(),
                what + ", Hs");
    } else if (doing == "fence" || doing == "unfence") {
      p->fenced = doing == "fence";
    } else if (doing == "erase") {
      p->nvs = false;  // the next boot draws the next epoch
    } else if (doing == "delete") {
      run.card.remove((kDir + st["name"].str()).c_str());
    } else if (doing == "damage") {
      Bytes b = bytesOf(run.card, st["name"].str());
      for (uint32_t s = 0; s < sf::kSecs; ++s)
        if (sf::kTypes[s] == cc::get32(reinterpret_cast<const uint8_t*>(st["section"].c_str()))) b[offsetOf(b, static_cast<sf::Sec>(s))] ^= 0xFF;
      put(run.card, {{st["name"].str(), b}});
    } else {
      // update (inert), remount (the session goes on), nvs (the seq rule's:
      // the slots' seqs come from the facts).
      TEST_ASSERT_TRUE_MESSAGE(doing == "update" || doing == "remount" || doing == "nvs", what.c_str());
    }
    if (p && st.has("records"))
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(st["records"].u64(), p->fold ? p->fold->records() : 0, what.c_str());
    if (p && st.has("behind"))
      TEST_ASSERT_EQUAL_MESSAGE(st["behind"].boolean(), p->fold && p->fold->behind(), what.c_str());
    sameFiles(out, filesOf(run.card), what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, run.card.violations.size(), what.c_str());
    ++*steps;
  }
}

}  // namespace

void test_every_session_the_store_runs() {
  int sessions = 0, steps = 0, saves = 0, held = 0, behind = 0;
  std::string skipped;
  for (size_t i = 0; i < gSessions.size(); ++i) {
    const minijson::Value& sc = gSessions[i];
    if (!storeRuns(sc)) {
      skipped += " " + sc["name"].str();
      continue;
    }
    SessionRun run;
    runSession(sc, run, &steps);
    ++sessions;
    saves += run.saves;
    held += run.held;
    behind += run.behind;
  }
  printf("sessions: %d through the store, %d steps, %d saves (%d held, %d behind); not run (a twin's link):%s\n",
         sessions, steps, saves, held, behind, skipped.c_str());
  TEST_ASSERT_EQUAL_INT(32, sessions);
  TEST_ASSERT_TRUE(held >= 4 && behind >= 3);
}

// replace() failing at a records' save and at a compacting boot's save, then
// the save that promotes it: the card as statsref's session has it, and as
// the same session where that save landed once.
void test_held_then_promoted_as_if_landed_once() {
  for (const char* name : {"records-save-held", "boot-compaction-held"}) {
    const minijson::Value& sc = session(name);
    // The session as stored: replace() failing once, the tmp held, then
    // promoted (statsref's card after each step: runSession()).
    SessionRun held;
    int steps = 0;
    runSession(sc, held, &steps);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, held.held, name);
    // The same saves with no replace() failing: each lands (their outcomes
    // aren't the session's, so they aren't compared), and the card at the
    // end is the held run's.
    SessionRun once;
    startSession(sc, once);
    SPlayer& a = once.players.at("A");
    for (size_t i = 0; i < sc["steps"].size(); ++i) {
      const minijson::Value& st = sc["steps"][i];
      const std::string doing = st["do"].str();
      const Files out = sessionCard(st["out"]);
      if (doing == "boot") {
        if (!a.nvs) {
          a.epoch = a.epochs.at(a.next++);
          a.nvs = true;
        }
        a.store.reset();
        a.fold.reset(new sf::Fold);
        a.store.reset(new sx::Store(once.fs));
        TEST_ASSERT_TRUE(sx::statsOn(a.store->read(*a.fold, once.w.p(), waitHook).read));
        Ranges rg;
        TEST_ASSERT_TRUE(rg.begin(*a.fold, acksOf(st["acks"])));
        scanLog(*a.fold, once.card, {});
        const sx::SaveReport r = once.save(a, st, true, false, false, st["now"]);
        TEST_ASSERT_TRUE_MESSAGE(sx::savedOk(r.what), name);
        trim(once.card, a.fold.get(), acksOf(st["acks"]), {});
      } else if (doing == "play" || doing == "plays") {
        const size_t n = doing == "play" ? 1 : st["each"]["seq"].size();
        for (size_t k = 0; k < n; ++k) {
          const minijson::Value& rec = doing == "play" ? st["recordsSave"] : st["each"]["recordsSave"][k];
          const minijson::Value& now = doing == "play" ? st["now"] : st["plays"]["now"][k];
          if (!rec.isNull())
            TEST_ASSERT_TRUE_MESSAGE(sx::savedOk(once.save(a, st, false, false, false, now).what), name);
          once.fold(a, st, doing == "play" ? SIZE_MAX : k, doing == "play" ? st["seq"] : st["each"]["seq"][k]);
        }
        once.logs(out);
      } else if (doing == "hs") {
        TEST_ASSERT_TRUE_MESSAGE(sx::savedOk(once.save(a, st, false, false, false, st["now"]).what), name);
      }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, once.held, name);
    sameFiles(filesOf(held.card), filesOf(once.card), std::string(name) + ": held then promoted, against landed once");
  }
}

// ---------------------------------------------------------------------------
// The boot's reads again, a remount's one.
// ---------------------------------------------------------------------------
namespace {
cutfs::CutFs* gCard = nullptr;
Bytes gGood;
void restoreGood() { gCard->put(sx::kStatsBin, std::vector<uint8_t>(gGood.begin(), gGood.end())); }
}  // namespace

void test_the_boot_reads_again() {
  const minijson::Value& b = scenario("worked-example")["boots"][0];
  const Files in = cardOf(b["in"]);
  Work w;
  struct Case {
    long from, to;  // stats.bin's events that fail (open, entry, reads)
    bool boot;      // the boot's hook (else a remount's: none)
    const char* read;
    std::vector<uint32_t> waits;
  };
  const Case cases[] = {
      {0, 1, true, "loaded", {100}},                  // the open, once
      {1, 3, true, "loaded", {100, 300}},             // the header's read, then the next open
      {0, -1, true, "unread", {100, 300, 1000}},      // for ever: 4 reads, stats off
      {27, -1, true, "unread", {100, 300, 1000}},     // HEAD's read (open 0, the frame 1-26)
      {0, 1, false, "unread", {}},                    // a remount: one read
  };
  for (const Case& c : cases) {
    cutfs::CutFs card;
    put(card, in);
    card.faultPrefix = sx::kStatsBin;
    card.faultFrom = c.from;
    card.faultTo = c.to;
    sf::Fold f;
    sx::Store store(card);
    gWaits.clear();
    const sx::Opened o = store.read(f, w.p(), c.boot ? waitHook : nullptr);
    TEST_ASSERT_EQUAL_STRING(c.read, sx::readName(o.read));
    TEST_ASSERT_EQUAL_UINT32(c.waits.size(), gWaits.size());
    for (size_t i = 0; i < c.waits.size(); ++i) TEST_ASSERT_EQUAL_UINT32(c.waits[i], gWaits[i]);
    TEST_ASSERT_EQUAL_UINT32(c.waits.size() + 1, o.reads);
    if (std::string(c.read) == "unread") {
      TEST_ASSERT_EQUAL_STRING("io", cc::whyName(o.why));
      TEST_ASSERT_FALSE(store.on());
      // Stats off: no save, the card as it was.
      TEST_ASSERT_TRUE(store.save(f, sx::SaveAsk(), w.p()).what == sx::Saved::Off);
      card.faultFrom = -1;
      sameFiles(in, filesOf(card), "unread");
    } else {
      TEST_ASSERT_EQUAL_UINT32(generationOf(in.at("stats.bin")), f.generation());
    }
  }
  // Damaged records: the file's fault, read once, Unread (never taken for
  // absent: the history isn't saved over).
  {
    cutfs::CutFs card;
    put(card, in);
    card.damagedPrefix = sx::kStatsBin;
    sf::Fold f;
    sx::Store store(card);
    gWaits.clear();
    const sx::Opened o = store.read(f, w.p(), waitHook);
    TEST_ASSERT_EQUAL_STRING("unread", sx::readName(o.read));
    TEST_ASSERT_EQUAL_STRING("fatRecords", cc::whyName(o.why));
    TEST_ASSERT_EQUAL_UINT32(0, gWaits.size());
  }
  // stats.tmp's question the card fails (settle() Unread): read again.
  {
    cutfs::CutFs card;
    put(card, in);
    card.faultPrefix = sx::kStatsTmp;
    card.faultFrom = 0;
    card.faultTo = 1;
    sf::Fold f;
    sx::Store store(card);
    gWaits.clear();
    const sx::Opened o = store.read(f, w.p(), waitHook);
    TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o.read));
    TEST_ASSERT_EQUAL_UINT32(1, gWaits.size());
  }
  // A corrupt read, then a whole one after 100 ms: loaded, nothing renamed.
  {
    cutfs::CutFs card;
    put(card, in);
    gGood = in.at("stats.bin");
    Bytes bad = gGood;
    bad[bad.size() / 2] ^= 0x40;
    card.put(sx::kStatsBin, std::vector<uint8_t>(bad.begin(), bad.end()));
    gCard = &card;
    gOnWait = restoreGood;
    sf::Fold f;
    sx::Store store(card);
    gWaits.clear();
    const sx::Opened o = store.read(f, w.p(), waitHook);
    gOnWait = nullptr;
    TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o.read));
    TEST_ASSERT_EQUAL_UINT32(2, o.reads);
    TEST_ASSERT_EQUAL_UINT32(1, gWaits.size());
    TEST_ASSERT_EQUAL_UINT32(100, gWaits[0]);
    TEST_ASSERT_FALSE(o.quarantined);
    sameFiles(in, filesOf(card), "a corrupt read, then a whole one");
  }
  // Corrupt at a remount: one read, left for the next boot's two: Unread.
  {
    cutfs::CutFs card;
    put(card, in);
    Bytes bad = in.at("stats.bin");
    bad.pop_back();
    card.put(sx::kStatsBin, std::vector<uint8_t>(bad.begin(), bad.end()));
    sf::Fold f;
    sx::Store store(card);
    const sx::Opened o = store.read(f, w.p(), nullptr);
    TEST_ASSERT_EQUAL_STRING("unread", sx::readName(o.read));
    TEST_ASSERT_EQUAL_STRING("fileBytes", cc::whyName(o.why));
    TEST_ASSERT_FALSE(o.quarantined);
    TEST_ASSERT_TRUE(card.dir.count(sx::kStatsBin) == 1 && card.dir.count(sx::kStatsBad) == 0);
    // The boot's: twice, then stats.bad.
    gWaits.clear();
    const sx::Opened p = store.read(f, w.p(), waitHook);
    TEST_ASSERT_EQUAL_STRING("corrupt", sx::readName(p.read));
    TEST_ASSERT_EQUAL_UINT32(2, p.reads);
    TEST_ASSERT_TRUE(p.quarantined);
    TEST_ASSERT_TRUE(store.on());
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bad") == bad);
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsBin));
  }
  // Every CRC before the order (statsref's read): a MARK whose CRC fails
  // under an HOUR out of order is SectionCrc; HOUR out of order alone, Order;
  // a HEAD under 128 bytes, Shape. Each Corrupt.
  {
    const Bytes good = in.at("stats.bin");
    std::vector<Sec> secs = sectionsOf(good, sx::kStatsSpec);
    secs[sf::kHour].data = hourRow(20, 1, 5) + hourRow(10, 1, 5);
    const Bytes order = frameOf(generationOf(good), secs);
    Bytes crc = order;
    crc[offsetOf(crc, sf::kMark)] ^= 1;
    std::vector<Sec> shortHead = sectionsOf(good, sx::kStatsSpec);
    shortHead[sf::kHead].data.resize(120);
    const std::pair<Bytes, const char*> files[] = {
        {order, "order"}, {crc, "sectionCrc"}, {frameOf(generationOf(good), shortHead), "shape"}};
    for (const auto& c : files) {
      cutfs::CutFs card;
      put(card, in);
      put(card, {{"stats.bin", c.first}});
      sf::Fold f;
      sx::Store store(card);
      const sx::Opened o = store.read(f, w.p(), nullptr);
      TEST_ASSERT_EQUAL_STRING(c.second, cc::whyName(o.why));
      TEST_ASSERT_EQUAL_STRING("unread", sx::readName(o.read));  // (a remount's: Corrupt left for the boot)
      TEST_ASSERT_EQUAL_UINT32(0, f.marks());
    }
  }
  // No work memory: no read.
  {
    cutfs::CutFs card;
    put(card, in);
    sf::Fold f;
    sx::Store store(card);
    TEST_ASSERT_EQUAL_STRING("noMemory", sx::readName(store.read(f, nullptr, waitHook).read));
    TEST_ASSERT_FALSE(store.on());
  }
}

// ---------------------------------------------------------------------------
// MARK's room: the object's 16 rows, a block for a card past 8 (the boot's,
// carved once and kept), never a carve at a remount.
// ---------------------------------------------------------------------------
void test_marks_block() {
  Work w;
  auto cardWith = [](uint32_t marks) {
    Bytes mark;
    for (uint32_t i = 0; i < marks; ++i) {
      uint8_t dev[6] = {0x00, 0x00, 0x5e, 0x00, 0x54, static_cast<uint8_t>(i)};
      mark += markRow(dev, kEpochA, 10 + i);
    }
    sf::Head h;
    h.sinceDay = 300;
    return statsFile(3, h, mark, "", "", "", "");
  };
  // 8 rows: the object's 16 hold them and 8 more.
  {
    cutfs::CutFs card;
    put(card, {{"stats.bin", cardWith(8)}});
    gHeap.carves = 0;
    sf::Fold f;
    sx::Store store(card, heapAlloc, heapFree);
    const sx::Opened o = store.read(f, w.p(), waitHook);
    TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o.read));
    TEST_ASSERT_EQUAL_UINT32(8, o.marks);
    TEST_ASSERT_EQUAL_UINT32(0, o.markBytes);
    TEST_ASSERT_EQUAL_UINT32(0, gHeap.carves);
    TEST_ASSERT_EQUAL_UINT32(sf::kMarkRows, f.markCap());
  }
  // 9 and 40 rows: the boot's read carves their rows and 8 more, once; a
  // second boot read keeps it; a save writes MARK from it byte for byte.
  for (uint32_t n : {9u, 40u}) {
    cutfs::CutFs card;
    const Bytes bin = cardWith(n);
    put(card, {{"stats.bin", bin}});
    gHeap.carves = 0;
    {
      sf::Fold f;
      sx::Store store(card, heapAlloc, heapFree);
      sx::Opened o = store.read(f, w.p(), waitHook);
      TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o.read));
      TEST_ASSERT_EQUAL_UINT32(sf::Fold::markBlockBytes(n), o.markBytes);
      TEST_ASSERT_EQUAL_UINT32(sf::Fold::markBlockBytes(n), store.markBytes());
      TEST_ASSERT_EQUAL_UINT32(n + sf::kMarkRoom, f.markCap());
      TEST_ASSERT_EQUAL_UINT32(n, f.marks());
      TEST_ASSERT_EQUAL_UINT32(1, gHeap.carves);
      o = store.read(f, w.p(), nullptr);  // a remount's: the block it has
      TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o.read));
      TEST_ASSERT_EQUAL_UINT32(1, gHeap.carves);
      // 8 new pairs fold; a ninth is the fold out of memory.
      Ranges rg;
      TEST_ASSERT_TRUE(rg.begin(f, cc::msmf::Acks()));
      for (uint32_t k = 0; k < sf::kMarkRoom + 1; ++k) {
        pl::Slot s = playA(1, hashOf(k), at(301, 10, k));
        s.deviceId[4] = 0x55;
        s.deviceId[5] = static_cast<uint8_t>(k);
        f.bootSlot(s, 0);
        TEST_ASSERT_EQUAL(k == sf::kMarkRoom, f.noMemory());
      }
      // (out of memory: no save; a fold within the room saves MARK whole)
      TEST_ASSERT_EQUAL_STRING("off", sx::savedName(store.save(f, sx::SaveAsk(), w.p()).what));
    }
    TEST_ASSERT_EQUAL_UINT32(0, gHeap.blocks.size());  // the store gave it back
    {
      sf::Fold f;
      sx::Store store(card, heapAlloc, heapFree);
      TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(store.read(f, w.p(), waitHook).read));
      Ranges rg;
      TEST_ASSERT_TRUE(rg.begin(f, cc::msmf::Acks()));
      for (uint32_t k = 0; k < sf::kMarkRoom; ++k) {
        pl::Slot s = playA(1, hashOf(k), at(301, 10, k));
        s.deviceId[4] = 0x55;
        s.deviceId[5] = static_cast<uint8_t>(k);
        f.bootSlot(s, 0);
      }
      f.endBoot();
      sx::SaveAsk ask;
      ask.nowMs = at(302, 9);
      const Expect e = expectSave(f, bin, "", 0, ask);
      const sx::SaveReport r = store.save(f, ask, w.p());
      TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(r.what));
      TEST_ASSERT_EQUAL_UINT32(n + sf::kMarkRoom, r.rows[sf::kMark]);
      TEST_ASSERT_TRUE_MESSAGE(bytesOf(card, "stats.bin") == e.bin, hex(bytesOf(card, "stats.bin"), e.bin).c_str());
    }
    TEST_ASSERT_EQUAL_UINT32(0, gHeap.blocks.size());
  }
  // A remount's read past the object's 16 with no block: no memory (the
  // stats off for the session), nothing carved, never a MARK cut short.
  {
    cutfs::CutFs card;
    put(card, {{"stats.bin", cardWith(17)}});
    gHeap.carves = 0;
    sf::Fold f;
    sx::Store store(card, heapAlloc, heapFree);
    const sx::Opened o = store.read(f, w.p(), nullptr);
    TEST_ASSERT_EQUAL_STRING("noMemory", sx::readName(o.read));
    TEST_ASSERT_FALSE(store.on());
    TEST_ASSERT_EQUAL_UINT32(0, gHeap.carves);
    TEST_ASSERT_EQUAL_UINT32(0, f.marks());
    // ... and 16 there: loaded (the object's rows).
    put(card, {{"stats.bin", cardWith(16)}});
    TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(store.read(f, w.p(), nullptr).read));
    TEST_ASSERT_EQUAL_UINT32(16, f.marks());
  }
  // The boot's carve failing: no memory, the stats off.
  {
    cutfs::CutFs card;
    put(card, {{"stats.bin", cardWith(12)}});
    gHeap.fail = true;
    sf::Fold f;
    sx::Store store(card, heapAlloc, heapFree);
    const sx::Opened o = store.read(f, w.p(), waitHook);
    gHeap.fail = false;
    TEST_ASSERT_EQUAL_STRING("noMemory", sx::readName(o.read));
    TEST_ASSERT_FALSE(store.on());
  }
}

// ---------------------------------------------------------------------------
// stats.bad, and the chains.
// ---------------------------------------------------------------------------
void test_stats_bad_never_shares_a_chain() {
  const minijson::Value& b = scenario("corrupt")["boots"][0];
  const Files in = cardOf(b["in"]);
  const Files want = cardOf(b["out"]);
  Work w;
  {
    // stats.bin on a twin's chain (a rename cut, settled): it goes to a
    // twin, not to stats.bad, which collectTwins() would one day free.
    cutfs::CutFs card;
    put(card, in);
    card.link(sx::kStatsBin, "/.player/stats.xl1");
    const BootOut r = fixtureBoot(card, card, b, w);
    TEST_ASSERT_EQUAL_STRING("corrupt", sx::readName(r.o.read));
    TEST_ASSERT_TRUE(r.o.quarantined);
    TEST_ASSERT_TRUE(r.s.what == sx::Saved::Wrote);
    TEST_ASSERT_EQUAL_UINT32(0, card.violations.size());
    TEST_ASSERT_TRUE(card.shared("/.player/stats.xl1", "/.player/stats.xl2"));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bad") == in.at("stats.bad"));  // the older one, left
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == want.at("stats.bin"));
    // A disk check parts them; the next settle() collects them.
    card.diskCheck();
    TEST_ASSERT_EQUAL_UINT32(0, tagstore::collectTwins(card, sx::kStatsNames));
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count("/.player/stats.xl1") + card.dir.count("/.player/stats.xl2"));
    TEST_ASSERT_EQUAL_UINT32(0, card.violations.size());
  }
  {
    // An older stats.bad on stats.bin's chain (this very rename, cut
    // between its two writes): both go to twins; nothing freed twice.
    cutfs::CutFs card;
    put(card, in);
    card.dir.erase(sx::kStatsBad);
    card.link(sx::kStatsBin, sx::kStatsBad);
    const BootOut r = fixtureBoot(card, card, b, w);
    TEST_ASSERT_TRUE(r.o.quarantined);
    TEST_ASSERT_TRUE(r.s.what == sx::Saved::Wrote);
    TEST_ASSERT_EQUAL_UINT32(0, card.violations.size());
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsBad));
    TEST_ASSERT_TRUE(card.shared("/.player/stats.xl1", "/.player/stats.xl2"));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == want.at("stats.bin"));
  }
  // A cut at each step of the read's renames and removes, then a boot: no
  // chain freed twice, stats.bin the fresh start's.
  int cuts = 0;
  for (long k = 1;; ++k) {
    cutfs::CutFs card;
    put(card, in);
    card.cutAt = k;
    {
      sf::Fold f;
      sx::Store store(card);
      store.read(f, w.p(), waitHook);
    }
    if (!card.dead) break;
    ++cuts;
    cutfs::CutFs next = card.reboot(cutfs::Variant::InOrder);
    const BootOut r = fixtureBoot(next, next, b, w);
    TEST_ASSERT_TRUE(statsOn(r.o.read));
    TEST_ASSERT_TRUE(r.s.what == sx::Saved::Wrote);
    TEST_ASSERT_EQUAL_UINT32(0, next.violations.size());
    TEST_ASSERT_TRUE(bytesOf(next, "stats.bin") == want.at("stats.bin"));
  }
  TEST_ASSERT_EQUAL_INT(3, cuts);  // the old stats.bad's remove, the rename's two writes
}

// ---------------------------------------------------------------------------
// The base.
// ---------------------------------------------------------------------------
void test_the_base_check() {
  const minijson::Value& b = scenario("base-neighbours")["boots"][0];
  const Files in = cardOf(b["in"]);
  const std::string base = baseName(1);
  Work w;
  struct Case {
    long from, to;
    bool damaged;
    const char* read;
    const char* why;
    size_t waits;
  };
  const Case cases[] = {
      {0, -1, false, "unread", "io", 3},          // the open, for ever
      {1, -1, false, "unread", "io", 3},          // the header's read, then every open
      {0, 1, false, "loaded", "ok", 1},           // the open, once
      {6, 7, false, "loaded", "ok", 1},           // the rows' read, once (open 0, header 1, the frame 2-5)
      {-1, -1, true, "unread", "fatRecords", 0},  // damaged records: no read again
  };
  for (const Case& c : cases) {
    cutfs::CutFs card;
    put(card, in);
    card.faultPrefix = "/.player/stats-00000001.bin";
    card.faultFrom = c.from;
    card.faultTo = c.to;
    if (c.damaged) card.damagedPrefix = "/.player/stats-00000001.bin";
    sf::Fold f;
    sx::Store store(card);
    gWaits.clear();
    const sx::Opened o = store.read(f, w.p(), waitHook);
    TEST_ASSERT_EQUAL_STRING(c.read, sx::readName(o.read));
    TEST_ASSERT_EQUAL_STRING(c.why, cc::whyName(o.why));
    TEST_ASSERT_EQUAL_UINT32(c.waits, gWaits.size());
    TEST_ASSERT_FALSE(o.baseLost);
    if (std::string(c.read) == "unread") {
      // Never a lost base, and nothing removed.
      TEST_ASSERT_TRUE(o.inBase);
      TEST_ASSERT_EQUAL_UINT32(0, o.removed);
      card.faultFrom = -1;
      card.damagedPrefix.clear();
      sameFiles(in, filesOf(card), c.why);
    } else {
      TEST_ASSERT_EQUAL_UINT32(1, o.removed);
      TEST_ASSERT_EQUAL_UINT32(1, store.cardCopy().baseGen);
    }
  }
  // At a remount: one read.
  {
    cutfs::CutFs card;
    put(card, in);
    card.faultPrefix = base;
    card.faultFrom = 0;
    card.faultTo = 1;
    sf::Fold f;
    sx::Store store(card);
    gWaits.clear();
    const sx::Opened o = store.read(f, w.p(), nullptr);
    TEST_ASSERT_EQUAL_STRING("unread", sx::readName(o.read));
    TEST_ASSERT_EQUAL_UINT32(1, o.baseReads);
  }
  // Its header the one named over rows out of order, or of another count:
  // Unread (the card's fault, never a lost base). A stats.bin naming a
  // base with another row count, its CRC made to match.
  const Bytes stats = in.at("stats.bin");
  std::vector<Sec> secs = sectionsOf(stats, sx::kStatsSpec);
  sf::Head h = sf::decodeHead(u8(secs[0].data));
  std::vector<Sec> rows = sectionsOf(in.at("stats-00000001.bin"), sx::kBaseSpec);
  TEST_ASSERT_TRUE(rows[0].data.size() >= 48);
  Bytes swapped = rows[0].data;
  std::swap_ranges(swapped.begin(), swapped.begin() + 24, swapped.begin() + 24);
  const Bytes outOfOrder = frameOf(1, {{sf::kTrks, cc::kSectionRequired, 24, swapped}});
  const Bytes fewer = frameOf(1, {{sf::kTrks, cc::kSectionRequired, 24, rows[0].data.substr(24)}});
  for (int which = 0; which < 2; ++which) {
    const Bytes& nb = which == 0 ? outOfOrder : fewer;
    sf::Head hh = h;
    hh.baseCrc = headerCrcOf(nb);
    uint8_t enc[sf::kHeadBytes];
    sf::encodeHead(hh, enc);
    std::vector<Sec> s2 = secs;
    s2[0].data = Bytes(reinterpret_cast<const char*>(enc), sizeof(enc));
    cutfs::CutFs card;
    put(card, in);
    put(card, {{"stats.bin", frameOf(generationOf(stats), s2)}, {"stats-00000001.bin", nb}});
    sf::Fold f;
    sx::Store store(card);
    gWaits.clear();
    const sx::Opened o = store.read(f, w.p(), waitHook);
    TEST_ASSERT_EQUAL_STRING("unread", sx::readName(o.read));
    TEST_ASSERT_EQUAL_STRING(which == 0 ? "order" : "counts", cc::whyName(o.why));
    TEST_ASSERT_EQUAL_UINT32(0, gWaits.size());
    TEST_ASSERT_FALSE(o.baseLost);
  }
}

// ---------------------------------------------------------------------------
// The commit.
// ---------------------------------------------------------------------------
void test_the_card_copy_is_checked_first() {
  const minijson::Value& sc = scenario("worked-example");
  const minijson::Value& b = sc["boots"][0];
  const Files in = cardOf(b["in"]);
  const Bytes other = cardOf(b["out"]).at("stats.bin");  // another player's save meanwhile
  Work w;
  // 0: another stats.bin; 1: stats.bin deleted; 2: none read, one there now.
  for (int c = 0; c < 3; ++c) {
    cutfs::CutFs card;
    Files start = in;
    if (c == 2) start.erase("stats.bin");
    put(card, start);
    sf::Fold f;
    sx::Store store(card);
    const sx::Opened o = store.read(f, w.p(), waitHook);
    TEST_ASSERT_TRUE(statsOn(o.read));
    Ranges rg;
    TEST_ASSERT_TRUE(rg.begin(f, acksOf(b["acks"])));
    scanLog(f, card, {});
    f.endBoot();
    if (c == 0 || c == 2) card.put(sx::kStatsBin, std::vector<uint8_t>(other.begin(), other.end()));
    if (c == 1) card.remove(sx::kStatsBin);
    const Files before = filesOf(card);
    const long steps = card.steps;
    sx::SaveReport r = store.save(f, askOf(b), w.p());
    TEST_ASSERT_EQUAL_STRING("changed", sx::savedName(r.what));
    TEST_ASSERT_TRUE(store.changed());
    TEST_ASSERT_TRUE(f.dirty());
    // Every later save this session: refused, the card not asked.
    r = store.save(f, askOf(b), w.p());
    TEST_ASSERT_EQUAL_STRING("changed", sx::savedName(r.what));
    TEST_ASSERT_EQUAL_INT32(steps, card.steps);
    sameFiles(before, filesOf(card), "changed");
    // The next boot (or a remount's read) stands on the card's copy again.
    store.read(f, w.p(), waitHook);
    TEST_ASSERT_FALSE(store.changed());
  }
}

void test_the_commit_steps() {
  const minijson::Value& b = scenario("worked-example")["boots"][0];
  const Files in = cardOf(b["in"]);
  const Files want = cardOf(b["out"]);
  Work w;
  // Nothing new: true, nothing written.
  {
    const minijson::Value& b2 = scenario("worked-example")["boots"][1];
    cutfs::CutFs card;
    put(card, cardOf(b2["in"]));
    const long before = card.steps;
    sf::Fold f;
    sx::Store store(card);
    const BootOut r = bootOnce(card, store, f, w.p(), acksOf(b2["acks"]), {}, askOf(b2));
    TEST_ASSERT_EQUAL_STRING("nothing new", sx::savedName(r.s.what));
    TEST_ASSERT_TRUE(sx::savedOk(r.s.what));
    TEST_ASSERT_EQUAL_INT32(before, card.steps);
  }
  // No work memory.
  {
    cutfs::CutFs card;
    put(card, in);
    sf::Fold f;
    sx::Store store(card);
    TEST_ASSERT_TRUE(statsOn(store.read(f, w.p(), waitHook).read));
    Ranges rg;
    TEST_ASSERT_TRUE(rg.begin(f, acksOf(b["acks"])));
    scanLog(f, card, {});
    TEST_ASSERT_EQUAL_STRING("no memory", sx::savedName(store.save(f, askOf(b), nullptr).what));
    TEST_ASSERT_TRUE(f.dirty());
  }
  // The tmp's size on the card isn't what was written: the tmp removed, false.
  {
    cutfs::CutFs card;
    put(card, in);
    Wrap fs(card);
    fs.sizeOff = -1;
    sf::Fold f;
    sx::Store store(fs);
    const BootOut r = bootOnce(card, store, f, w.p(), acksOf(b["acks"]), {}, askOf(b));
    TEST_ASSERT_EQUAL_STRING("failed", sx::savedName(r.s.what));
    TEST_ASSERT_EQUAL_STRING("size", r.s.step);
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsTmp));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == in.at("stats.bin"));
    TEST_ASSERT_TRUE(f.dirty());
  }
  // replace() fails at its rename, stats.bin removed: the tmp held, whole;
  // RAM takes the plan (the records emptied), the card's MARK the one
  // before. A reboot's settle() promotes it (stats.bin gone). In the
  // session, the next save promotes it first (the card's MARK then the held
  // copy's), then writes the generation after it, the newer record alone
  // merged onto it.
  pl::Slot live;
  TEST_ASSERT_TRUE(pl::decode(u8(in.at("plays.jnl")), pl::kSlot, &live) == pl::Verdict::Ok);
  live.seq = 49;
  {
    cutfs::CutFs card;
    put(card, in);
    Wrap fs(card);
    fs.failTmpRenames = 1;
    sf::Fold f;
    sx::Store store(fs);
    const BootOut r = bootOnce(card, store, f, w.p(), acksOf(b["acks"]), {}, askOf(b));
    TEST_ASSERT_EQUAL_STRING("held", sx::savedName(r.s.what));
    TEST_ASSERT_TRUE(store.held());
    TEST_ASSERT_TRUE(f.heldPending());
    TEST_ASSERT_EQUAL_UINT32(0, f.records());
    TEST_ASSERT_FALSE(f.dirty());
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsBin));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.tmp") == want.at("stats.bin"));
    // (The trim followed the MARK read at the boot: plays.1.jnl kept.)
    TEST_ASSERT_EQUAL_UINT32(0, r.trimmed.size());
    {
      cutfs::CutFs next = card.reboot(cutfs::Variant::LoseUnsynced);
      sf::Fold g;
      sx::Store s2(next);
      const sx::Opened o = s2.read(g, w.p(), waitHook);
      TEST_ASSERT_TRUE(o.settled.what == tagstore::Settle::Promoted);
      TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o.read));
      TEST_ASSERT_TRUE(bytesOf(next, "stats.bin") == want.at("stats.bin"));
    }
    // A live play, then the next save (a rotation's): byte for byte the save
    // of that record over the held copy, read as a boot reads it.
    f.liveSlot(live);
    sx::SaveAsk ask = askOf(b);
    ask.mayCompact = false;
    const Expect e = expectSave(f, want.at("stats.bin"), "", 0, ask);
    const sx::SaveReport q = store.save(f, ask, w.p());
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(q.what));
    TEST_ASSERT_TRUE(q.heldPromoted);
    TEST_ASSERT_FALSE(store.held());
    TEST_ASSERT_EQUAL_UINT32(generationOf(want.at("stats.bin")) + 1, q.generation);
    TEST_ASSERT_EQUAL_UINT32(q.generation, generationOf(bytesOf(card, "stats.bin")));
    TEST_ASSERT_EQUAL_UINT32(q.generation, f.generation());
    TEST_ASSERT_EQUAL_UINT32(49, f.markOf(live.deviceId, live.epoch));
    TEST_ASSERT_TRUE(f.covered(live));
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsTmp));
    TEST_ASSERT_EQUAL_UINT32(0, card.violations.size());
    TEST_ASSERT_TRUE_MESSAGE(bytesOf(card, "stats.bin") == e.bin, hex(bytesOf(card, "stats.bin"), e.bin).c_str());
  }
  // replace() fails at its remove: stats.bin as it was, the tmp held. A
  // remount's read promotes it first (settle() would have dropped it).
  {
    cutfs::CutFs card;
    put(card, in);
    Wrap fs(card);
    fs.failBinRemoves = 1;
    sf::Fold f;
    sx::Store store(fs);
    const BootOut r = bootOnce(card, store, f, w.p(), acksOf(b["acks"]), {}, askOf(b));
    TEST_ASSERT_EQUAL_STRING("held", sx::savedName(r.s.what));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == in.at("stats.bin"));
    const sx::Opened o = store.read(f, w.p(), nullptr);
    TEST_ASSERT_TRUE(o.heldPromoted);
    TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o.read));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == want.at("stats.bin"));
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsTmp));
    TEST_ASSERT_FALSE(store.held());
  }
  // A held tmp, then nothing new: the next save promotes it all the same
  // (the held tmp is something new), and writes nothing more.
  {
    cutfs::CutFs card;
    put(card, in);
    Wrap fs(card);
    fs.failBinRemoves = 1;
    sf::Fold f;
    sx::Store store(fs);
    bootOnce(card, store, f, w.p(), acksOf(b["acks"]), {}, askOf(b));
    TEST_ASSERT_TRUE(store.held());
    TEST_ASSERT_FALSE(f.covered(live));
    sx::SaveAsk ask = askOf(b);
    ask.mayCompact = false;
    const sx::SaveReport q = store.save(f, ask, w.p());
    TEST_ASSERT_EQUAL_STRING("nothing new", sx::savedName(q.what));
    TEST_ASSERT_TRUE(q.heldPromoted);
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == want.at("stats.bin"));
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsTmp));
    // The card's MARK the held copy's: the log's slots the boot folded covered.
    pl::Slot s;
    TEST_ASSERT_TRUE(pl::decode(u8(in.at("plays.jnl")), pl::kSlot, &s) == pl::Verdict::Ok);
    TEST_ASSERT_TRUE(f.covered(s));
  }
  // The held tmp lost: gone with stats.bin the copy before it (a PC,
  // another player's settle()), or a tmp no longer ours under its name:
  // the session is behind (RAM no longer holds the slots it took), the card
  // as it is. Gone with stats.bin the held copy (another's settle()
  // promoted it): promoted. Gone with stats.bin another: changed.
  for (int c = 0; c < 4; ++c) {
    cutfs::CutFs card;
    put(card, in);
    Wrap fs(card);
    fs.failBinRemoves = 1;
    sf::Fold f;
    sx::Store store(fs);
    bootOnce(card, store, f, w.p(), acksOf(b["acks"]), {}, askOf(b));
    TEST_ASSERT_TRUE(store.held());
    const Bytes theirs = cardOf(scenario("two-devices")["boots"][0]["out"]).at("stats.bin");
    if (c == 0) card.remove(sx::kStatsTmp);
    if (c == 1) card.put(sx::kStatsTmp, std::vector<uint8_t>(theirs.begin(), theirs.end()));
    if (c == 2) {
      card.remove(sx::kStatsBin);
      card.rename(sx::kStatsTmp, sx::kStatsBin);
    }
    if (c == 3) {
      card.remove(sx::kStatsTmp);
      card.put(sx::kStatsBin, std::vector<uint8_t>(theirs.begin(), theirs.end()));
    }
    f.liveSlot(live);
    const Files before = filesOf(card);
    const sx::SaveReport q = store.save(f, askOf(b), w.p());
    const char* expect = c < 2 ? "behind" : c == 2 ? "wrote" : "changed";
    TEST_ASSERT_EQUAL_STRING(expect, sx::savedName(q.what));
    TEST_ASSERT_EQUAL(c < 2, f.behind());
    TEST_ASSERT_FALSE(store.held());
    if (c != 2) sameFiles(before, filesOf(card), expect);
    if (c == 2) TEST_ASSERT_TRUE(q.heldPromoted && f.covered(live));
    if (c < 2) {
      TEST_ASSERT_EQUAL_STRING("held tmp lost", q.step);
      TEST_ASSERT_EQUAL_UINT32(0, f.records());
      TEST_ASSERT_EQUAL_STRING("behind", sx::savedName(store.save(f, askOf(b), w.p()).what));
      TEST_ASSERT_FALSE(f.liveSlot(live));
    }
  }
  // A full card: every step fails, the card as it was.
  {
    cutfs::CutFs card;
    put(card, in);
    sf::Fold f;
    sx::Store store(card);
    const BootOut r = bootOnce(card, store, f, w.p(), acksOf(b["acks"]), {}, askOf(b), true);
    TEST_ASSERT_EQUAL_STRING("failed", sx::savedName(r.s.what));
    TEST_ASSERT_EQUAL_STRING("tmp", r.s.step);
    TEST_ASSERT_FALSE(store.held());
    TEST_ASSERT_TRUE(f.dirty());
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == in.at("stats.bin"));
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsTmp));
  }
  // A partial scan: no save (Off).
  {
    const minijson::Value& p = scenario("partial-open")["boots"][0];
    cutfs::CutFs card;
    put(card, cardOf(p["in"]));
    sf::Fold f;
    sx::Store store(card);
    const BootOut r = bootOnce(card, store, f, w.p(), acksOf(p["acks"]), unreadOf(p), askOf(p));
    TEST_ASSERT_EQUAL_STRING("off", sx::savedName(r.s.what));
    TEST_ASSERT_FALSE(sx::savedOk(r.s.what));
  }
}

// A fold that ran out of memory is never saved, with the save's work memory
// or without it (5.5: the trim then follows the MARK read at the boot). One
// that ran out before its first fold (no ranges' block) isn't dirty, and the
// work memory, carved from the same short heap, may be missing too: still no
// save, never "nothing new" (the journal would trim every file ACKS
// acknowledges, plays never folded among them). Nor a partial scan's that
// folded nothing. Over a loaded stats.bin and over none (absent).
void test_a_fold_out_of_memory_is_never_saved() {
  const minijson::Value& b = scenario("worked-example")["boots"][0];
  const Files in = cardOf(b["in"]);
  Work w;
  int runs = 0;
  for (int absent = 0; absent < 2; ++absent) {
    for (int partial = 0; partial < 2; ++partial) {
      for (int withWork = 0; withWork < 2; ++withWork) {
        char what[96];
        std::snprintf(what, sizeof(what), "%s, %s, %s", absent ? "absent" : "loaded",
                      partial ? "every log file unread" : "no memory for the boot's fold",
                      withWork ? "work memory" : "no work memory");
        cutfs::CutFs card;
        put(card, in);
        if (absent) card.remove((kDir + "stats.bin").c_str());
        sf::Fold f;
        sx::Store store(card);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(absent ? "absent" : "loaded", sx::readName(store.read(f, w.p(), waitHook).read),
                                         what);
        std::map<std::string, int> unread;
        Ranges rg;
        if (partial) {
          for (const char* n : kLogNames)
            if (card.dir.count(kDir + n)) unread[n] = -1;
          TEST_ASSERT_TRUE_MESSAGE(rg.begin(f, acksOf(b["acks"])), what);
        } else {
          TEST_ASSERT_FALSE_MESSAGE(f.beginBoot(acksOf(b["acks"]), nullptr, 0), what);
        }
        scanLog(f, card, unread);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, f.folded(), what);
        TEST_ASSERT_FALSE_MESSAGE(f.dirty(), what);
        TEST_ASSERT_TRUE_MESSAGE(partial ? f.partialScan() : f.noMemory(), what);
        const Files before = filesOf(card);
        const long steps = card.steps;
        const sx::SaveReport r = store.save(f, askOf(b), withWork ? w.p() : nullptr);
        TEST_ASSERT_FALSE_MESSAGE(sx::savedOk(r.what), (std::string(what) + ": " + sx::savedName(r.what)).c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE("off", sx::savedName(r.what), what);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(steps, card.steps, what);
        sameFiles(before, filesOf(card), what);
        ++runs;
      }
    }
  }
  TEST_ASSERT_EQUAL_INT(8, runs);
}

void test_a_cut_at_every_step_of_a_save() {
  // Each step of the boot's save cut, three ways; the next boot then finds
  // the old commit or the new one, folds again what the old one lacks,
  // and leaves the card as the uncut boot did: no play lost or counted
  // twice, no base lost, no chain freed while another entry holds it.
  Work w;
  // base-other's first boot doesn't compact (its name is the one stats.bin
  // names); its second compacts over that file, named by nothing then.
  const std::pair<const char*, int> boots[] = {{"compaction", 0}, {"rules", 0},      {"worked-example", 0},
                                               {"corrupt", 0},    {"base-other", 0}, {"base-other", 1}};
  int runs = 0;
  for (const auto& which : boots) {
    const char* name = which.first;
    const minijson::Value& b = scenario(name)["boots"][which.second];
    const Files in = cardOf(b["in"]);
    const Files want = cardOf(b["out"]);
    int steps = 0;
    for (long k = 1;; ++k) {
      bool cut = false;
      for (cutfs::Variant v : {cutfs::Variant::InOrder, cutfs::Variant::LoseUnsynced, cutfs::Variant::Torn}) {
        cutfs::CutFs card;
        put(card, in);
        sf::Fold f;
        sx::Store store(card);
        const sx::Opened o = store.read(f, w.p(), waitHook);
        TEST_ASSERT_TRUE(statsOn(o.read));
        Ranges rg;
        TEST_ASSERT_TRUE(rg.begin(f, acksOf(b["acks"])));
        scanLog(f, card, {});
        f.endBoot();
        card.cutAt = card.steps + k;
        const sx::SaveReport r = store.save(f, askOf(b), w.p());
        if (!card.dead) {
          TEST_ASSERT_EQUAL_STRING_MESSAGE("wrote", sx::savedName(r.what), name);
          break;
        }
        cut = true;
        TEST_ASSERT_EQUAL_UINT32(0, card.violations.size());
        cutfs::CutFs next = card.reboot(v);
        char what[96];
        std::snprintf(what, sizeof(what), "%s %d, cut at the save's step %ld, way %d", name, which.second + 1, k,
                      static_cast<int>(v));
        const BootOut again = fixtureBoot(next, next, b, w);
        // The old commit or the new one (none yet: absent), never unread.
        const std::string read = sx::readName(again.o.read);
        TEST_ASSERT_TRUE_MESSAGE(read == "loaded" || (read == "absent" && b["read"].str() != "loaded"), what);
        // A base lost only where the boot's own read lost it (base-other).
        const bool lostBefore = (b["stats"]["head"]["flags"].u64() & sf::kFlagBaseLost) != 0;
        TEST_ASSERT_TRUE_MESSAGE(!again.o.baseLost || lostBefore, what);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, next.violations.size(), what);
        // A cut between the rename's two writes leaves stats.tmp on
        // stats.bin's chain: settle() makes it a twin, kept until a disk
        // check parts them (2.12.6), then collected.
        for (int t = 1; t <= tagstore::kMaxTwins; ++t) {
          char twin[sx::kNameCap];
          TEST_ASSERT_TRUE(tagstore::twinName(sx::kStatsNames, t, twin, sizeof(twin)));
          if (next.dir.count(twin)) TEST_ASSERT_TRUE_MESSAGE(next.shared(twin, sx::kStatsBin), what);
        }
        next.diskCheck();
        tagstore::collectTwins(next, sx::kStatsNames);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, next.violations.size(), what);
        sameFiles(want, filesOf(next), what);
        ++runs;
      }
      if (!cut) break;
      ++steps;
    }
    printf("%s %d: the save's %d steps cut three ways\n", name, which.second + 1, steps);
    TEST_ASSERT_TRUE(steps >= 6);
  }
  TEST_ASSERT_TRUE(runs > 0);
}

// ---------------------------------------------------------------------------
// Each cut at its edge, and the compaction's due edge, against the merge in
// miniature and statsref's retention.
// ---------------------------------------------------------------------------
namespace {
uint32_t gPeakOpen = 0;  // saveAgainst()'s card: the most files open at once
// One save of `bin` (base 1 `base`) with `slots` folded live, against the
// reference; the file's rows returned.
sx::SaveReport saveAgainst(const Bytes& bin, const Bytes& base, const std::vector<pl::Slot>& slots,
                           const sx::SaveAsk& ask, const char* what, Expect* e = nullptr) {
  Work w;
  cutfs::CutFs card;
  sf::Fold f;
  sx::Store store(card);
  loadAndFold(card, store, f, w, bin, base, slots);
  const Expect x = expectSave(f, bin, base, 0, ask);
  card.peakOpen = card.openNow;
  const sx::SaveReport r = store.save(f, ask, w.p());
  gPeakOpen = card.peakOpen;
  TEST_ASSERT_EQUAL_STRING_MESSAGE("wrote", sx::savedName(r.what), what);
  const Bytes got = bytesOf(card, "stats.bin");
  TEST_ASSERT_TRUE_MESSAGE(got == x.bin, (std::string(what) + ": stats.bin, " + hex(got, x.bin)).c_str());
  TEST_ASSERT_EQUAL_MESSAGE(x.compacted, r.compacted, what);
  if (x.compacted) {
    const Bytes nb = bytesOf(card, "stats-00000002.bin");
    TEST_ASSERT_TRUE_MESSAGE(nb == x.base, (std::string(what) + ": the base, " + hex(nb, x.base)).c_str());
  }
  for (int i = 0; i < sf::kSecs; ++i) TEST_ASSERT_EQUAL_UINT32_MESSAGE(x.rows[i], r.rows[i], what);
  if (e) *e = x;
  return r;
}
}  // namespace

void test_the_cuts_at_their_edges() {
  sf::Head h;
  h.sinceDay = 100;
  h.round = 7;
  const Bytes mark = markRow(kDevA, kEpochA, 1000);
  sx::SaveAsk ask;
  ask.nowMs = at(310, 12);
  // HOUR: 839 rows and one new hour, 840 kept; 840 and one new, the oldest
  // goes; 840 and a play in a held hour, 840; 840 and an hour older than
  // every row, that hour goes.
  {
    Bytes hours;
    for (uint32_t k = 0; k < sf::kHourRows; ++k) hours += hourRow(1000 + k, 1, 60000);
    const uint64_t newHour = sf::kMinMs + 2000ull * sf::kHourMs;
    const uint64_t heldHour = sf::kMinMs + 1500ull * sf::kHourMs + 60000;
    const uint64_t oldHour = sf::kMinMs + 10ull * sf::kHourMs;
    const Bytes h839 = hours.substr(8);  // hours 1001-1839
    struct Case {
      const Bytes* rows;
      uint64_t when;
      uint32_t kept;
      uint32_t first;  // the first hour kept
    };
    const Case cases[] = {{&h839, newHour, 840, 1001},
                          {&hours, newHour, 840, 1001},
                          {&hours, heldHour, 840, 1000},
                          {&hours, oldHour, 840, 1000}};
    for (const Case& c : cases) {
      const Bytes bin = statsFile(3, h, mark, *c.rows, "", "", "");
      Expect e;
      const sx::SaveReport r = saveAgainst(bin, "", {playA(1001, 77, c.when)}, ask, "HOUR's edge", &e);
      TEST_ASSERT_EQUAL_UINT32(c.kept, r.rows[sf::kHour]);
      const std::vector<Sec> s = sectionsOf(e.bin, sx::kStatsSpec);
      TEST_ASSERT_EQUAL_UINT32(c.first, cc::get32(u8(s[sf::kHour].data)) & 0xFFFFFF);
    }
  }
  // DAYS: 399 rows and a new day, 400; 400 and a new day, 401: the oldest goes.
  {
    Bytes days;
    for (uint32_t k = 0; k < sf::kDaysRows; ++k) days += daysRow(static_cast<uint16_t>(k), 2, 300000);
    const Bytes d399 = days.substr(16);
    for (const Bytes* rows : {&d399, static_cast<const Bytes*>(&days)}) {
      const Bytes bin = statsFile(3, h, mark, "", *rows, "", "");
      Expect e;
      const sx::SaveReport r = saveAgainst(bin, "", {playA(1001, 77, at(500, 9))}, ask, "DAYS' edge", &e);
      TEST_ASSERT_EQUAL_UINT32(sf::kDaysRows, r.rows[sf::kDays]);
      const std::vector<Sec> s = sectionsOf(e.bin, sx::kStatsSpec);
      TEST_ASSERT_EQUAL_UINT32(1, cc::get16(u8(s[sf::kDays].data)));
    }
  }
  // RCNT's 4,096 rows: 4,095 and one new row, 4,096 kept; 4,096 and one
  // new, 4,097: the oldest day goes whole (a day across the edge, 50 rows),
  // 4,047 left; the oldest day one row, the rest 4,096 exactly: that row
  // goes, the next day kept; 4,096 and a play of a row already there,
  // 4,096; one day of 4,097 rows: it goes whole, none left. The clock on day 310, DAYS'
  // newest 302: nothing is older than its 32 days.
  {
    const Bytes days = daysRow(300, 1, 1) + daysRow(301, 1, 1) + daysRow(302, 1, 1);
    auto rcnt = [](uint32_t d300, uint32_t d301, uint32_t d302) {
      Bytes r;
      uint32_t i = 0;
      for (uint32_t k = 0; k < d300; ++k) r += rcntRow(300, 1, 1000000 + i++);
      for (uint32_t k = 0; k < d301; ++k) r += rcntRow(301, 1, 1000000 + i++);
      for (uint32_t k = 0; k < d302; ++k) r += rcntRow(302, 1, 1000000 + i++);
      return r;
    };
    struct Case {
      Bytes rows;
      pl::Slot play;
      uint32_t kept;
      const char* what;
    };
    const Case cases[] = {
        {rcnt(50, 4000, 45), playA(1001, 7, at(302, 9)), 4096, "4,095 and one new: 4,096"},
        {rcnt(50, 4000, 46), playA(1001, 7, at(302, 9)), 4047, "4,096 and one new: the oldest day goes"},
        {rcnt(1, 4000, 95), playA(1001, 7, at(302, 9)), 4096, "the rows from day 301 on 4,096: day 300 goes"},
        {rcnt(50, 4000, 46), playA(1001, 1000000 + 4050, at(302, 9)), 4096, "4,096 and a row there: 4,096"},
        {rcnt(0, 0, 4097), playA(1001, 1000000, at(302, 9)), 0, "one day of 4,097: it goes whole"},
    };
    for (const Case& c : cases) {
      const Bytes bin = statsFile(3, h, mark, "", days, c.rows, "");
      const sx::SaveReport r = saveAgainst(bin, "", {c.play}, ask, c.what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(c.kept, r.rows[sf::kRcnt], c.what);
    }
  }
  // RCNT's 32 days: rows on days 250-305, DAYS' newest 302. The clock on
  // day 310: from the earlier, 302, back 32 days: 270 goes, 271 stays, and
  // 303-305 (ahead of DAYS) stay. The clock on day 300 (DAYS a day ahead of
  // it: 302): from 300: 268 goes, 269 stays, the rows ahead too. The clock
  // unset: nothing by date.
  {
    Bytes rc;
    for (uint32_t d = 250; d <= 305; ++d) rc += rcntRow(static_cast<uint16_t>(d), 1, 5000 + d);
    const Bytes days = daysRow(290, 1, 1) + daysRow(302, 1, 1);
    const Bytes bin = statsFile(3, h, mark, "", days, rc, "");
    struct Case {
      uint64_t now;
      uint32_t first;  // the first day kept
      const char* what;
    };
    const Case cases[] = {{at(310, 12), 271, "the clock on day 310"},
                          {at(300, 12), 269, "the clock on day 300, DAYS a day ahead"},
                          {0, 250, "the clock unset"}};
    for (const Case& c : cases) {
      sx::SaveAsk a;
      a.nowMs = c.now;
      Expect e;
      // An undated play: the records give RCNT and DAYS nothing.
      const sx::SaveReport r = saveAgainst(bin, "", {playA(1001, 9, 0)}, a, c.what, &e);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(305 - c.first + 1, r.rows[sf::kRcnt], c.what);
      const std::vector<Sec> s = sectionsOf(e.bin, sx::kStatsSpec);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(c.first, cc::get16(u8(s[sf::kRcnt].data)), c.what);
    }
  }
}

// ---------------------------------------------------------------------------
// A failure in either pass: a read the card fails (Io) is a save that
// failed, tried again (the tmp gone, the card's file untouched); a byte a
// read gives other than written (a section damaged under the session) is
// Damaged: the session behind, the file as it is.
// ---------------------------------------------------------------------------
namespace {
// One save to sweep: a card, the boot's fold of its log (with ACKS's trim
// after), and live slots after it.
struct Sweep {
  std::string name;
  Files in;
  cc::msmf::Acks acks;
  sx::SaveAsk ask;
  std::vector<pl::Slot> live;
};

// The boot up to its save, over `fs` (a Wrap over `card`).
void upToTheSave(const Sweep& c, cutfs::CutFs& card, sx::Store& store, sf::Fold& f, Work& w, Ranges& rg) {
  put(card, c.in);
  TEST_ASSERT_TRUE(statsOn(store.read(f, w.p(), waitHook).read));
  TEST_ASSERT_TRUE(rg.begin(f, c.acks));
  scanLog(f, card, {});
  f.endBoot();
  for (const pl::Slot& s : c.live) TEST_ASSERT_TRUE(f.liveSlot(s));
}

void sweep(const Sweep& c) {
  Work w;
  const char* name = c.name.c_str();
  const Bytes bin = c.in.at("stats.bin");
  // The save whole: the card it leaves (the trim after), the questions it
  // asks of stats.bin (opens, reads, entries) and its reads; then with its
  // compaction failing (the new base not created): stats.bin uncompacted.
  Files want;
  Bytes uncompacted;
  long reads = 0, events = 0;
  bool compacts = false;
  for (int k = 0; k < 2; ++k) {
    cutfs::CutFs card;
    Wrap fs(card);
    sf::Fold f;
    sx::Store store(fs);
    Ranges rg;
    upToTheSave(c, card, store, f, w, rg);
    fs.flipPath = sx::kStatsBin;
    fs.reads = 0;
    card.faultPrefix = sx::kStatsBin;
    card.faultEvents = 0;
    card.faultFrom = 1L << 30;  // (counted, none failing)
    if (k == 1) fs.failCreate = "/.player/stats-";
    const sx::SaveReport r = store.save(f, c.ask, w.p());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("wrote", sx::savedName(r.what), name);
    if (k == 0) {
      trim(card, &f, c.acks, {});
      want = filesOf(card);
      reads = fs.reads;
      events = card.faultEvents;
      compacts = r.compacted;
    } else {
      uncompacted = bytesOf(card, "stats.bin");
    }
  }
  // Each question of stats.bin failing once (the open, the header's, each
  // pass's reads, settle()'s and replace()'s questions): failed, the card
  // untouched, then the same save again: the whole save's card. Or, in the
  // compaction (stats.bin's TRKD), the compaction fails and the save goes
  // on; or at replace() (its question of stats.bin), the tmp held.
  int failedCount = 0, afterTmp = 0, heldCount = 0, inCompaction = 0;
  for (long k = 0; k < events; ++k) {
    cutfs::CutFs card;
    Wrap fs(card);
    sf::Fold f;
    sx::Store store(fs);
    Ranges rg;
    upToTheSave(c, card, store, f, w, rg);
    card.faultPrefix = sx::kStatsBin;
    card.faultEvents = 0;
    card.faultFrom = k;
    card.faultTo = k + 1;
    fs.creates.clear();
    const sx::SaveReport r = store.save(f, c.ask, w.p());
    card.faultFrom = -1;
    char what[96];
    std::snprintf(what, sizeof(what), "%s, stats.bin's question %ld failing", name, k);
    if (r.what == sx::Saved::Wrote) {
      TEST_ASSERT_TRUE_MESSAGE(r.compactFailed, what);
      TEST_ASSERT_TRUE_MESSAGE(bytesOf(card, "stats.bin") == uncompacted, what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.dir.count(baseName(2)), what);
      ++inCompaction;
      continue;
    }
    if (r.what == sx::Saved::Held) {
      TEST_ASSERT_TRUE_MESSAGE(bytesOf(card, "stats.tmp") == want.at("stats.bin"), what);
      TEST_ASSERT_TRUE_MESSAGE(bytesOf(card, "stats.bin") == bin, what);
      ++heldCount;
      continue;
    }
    TEST_ASSERT_EQUAL_STRING_MESSAGE("failed", sx::savedName(r.what), what);
    ++failedCount;
    afterTmp += std::count(fs.creates.begin(), fs.creates.end(), std::string(sx::kStatsTmp)) > 0;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.dir.count(sx::kStatsTmp), what);
    TEST_ASSERT_TRUE_MESSAGE(bytesOf(card, "stats.bin") == bin, what);
    TEST_ASSERT_TRUE_MESSAGE(f.dirty() && !f.behind(), what);
    if (c.in.count("stats-00000001.bin"))
      TEST_ASSERT_TRUE_MESSAGE(bytesOf(card, "stats-00000001.bin") == c.in.at("stats-00000001.bin"), what);
    // (A base the compaction wrote before a later read failed: named by
    // nothing, replaced by the next one's create, or a boot's neighbour.)
    if (card.dir.count(kDir + "stats-00000002.bin")) TEST_ASSERT_TRUE_MESSAGE(r.compacted, what);
    const sx::SaveReport again = store.save(f, c.ask, w.p());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("wrote", sx::savedName(again.what), what);
    trim(card, &f, c.acks, {});
    sameFiles(want, filesOf(card), what);
  }
  printf("%s: stats.bin's %ld questions each failing once: %d failed (%d after the tmp's create), %d in the "
         "compaction (the save went on), %d at replace() (held)\n",
         name, events, failedCount, afterTmp, inCompaction, heldCount);
  TEST_ASSERT_EQUAL_INT_MESSAGE(events, failedCount + inCompaction + heldCount, name);
  TEST_ASSERT_TRUE_MESSAGE(failedCount > 8 && heldCount == 1, name);
  // One byte of each section flipped in one read (the damage seen by that
  // read alone): Damaged (the section named), the session behind, no tmp,
  // the card as it was; or a read it isn't in (or one that has it before
  // its start, a refill from the sector, never passed), or the compaction's
  // (it fails, the save goes on uncompacted): the save lands.
  int damaged = 0, damagedAfterTmp = 0, flippedLanded = 0;
  const std::vector<Sec> secs = sectionsOf(bin, sx::kStatsSpec);
  for (uint32_t s = 0; s < sf::kSecs; ++s) {
    if (secs[s].data.empty()) continue;
    for (long k = 0; k < reads; ++k) {
      cutfs::CutFs card;
      Wrap fs(card);
      sf::Fold f;
      sx::Store store(fs);
      Ranges rg;
      upToTheSave(c, card, store, f, w, rg);
      fs.flipPath = sx::kStatsBin;
      fs.flipAt = offsetOf(bin, static_cast<sf::Sec>(s));
      fs.flipRead = k;
      fs.reads = 0;
      fs.creates.clear();
      const sx::SaveReport r = store.save(f, c.ask, w.p());
      char what[96];
      std::snprintf(what, sizeof(what), "%s, %.4s's first byte flipped in read %ld", name,
                    reinterpret_cast<const char*>(&sf::kTypes[s]), k);
      if (r.what == sx::Saved::Wrote) {
        const Bytes got = bytesOf(card, "stats.bin");
        TEST_ASSERT_TRUE_MESSAGE(got == (r.compactFailed ? uncompacted : want.at("stats.bin")), what);
        flippedLanded += fs.flipped && r.compactFailed;
        continue;
      }
      TEST_ASSERT_EQUAL_STRING_MESSAGE("damaged", sx::savedName(r.what), what);
      TEST_ASSERT_EQUAL_HEX32_MESSAGE(sf::kTypes[s], r.section, what);
      TEST_ASSERT_TRUE_MESSAGE(f.behind(), what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, f.records(), what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.dir.count(sx::kStatsTmp), what);
      TEST_ASSERT_TRUE_MESSAGE(bytesOf(card, "stats.bin") == bin, what);
      damagedAfterTmp += std::count(fs.creates.begin(), fs.creates.end(), std::string(sx::kStatsTmp)) > 0;
      ++damaged;
      // Every later save: behind, the card not asked.
      const long steps = card.steps;
      TEST_ASSERT_EQUAL_STRING_MESSAGE("behind", sx::savedName(store.save(f, c.ask, w.p()).what), what);
      TEST_ASSERT_EQUAL_INT32_MESSAGE(steps, card.steps, what);
    }
  }
  printf("%s: %d damaged sections seen (%d in the writing pass), %d flips in the compaction's read (it failed, the "
         "save went on)\n",
         name, damaged, damagedAfterTmp, flippedLanded);
  TEST_ASSERT_TRUE_MESSAGE(damaged >= 2, name);
  // Where the compaction reads stats.bin's TRKD, the faults there; where the
  // writing pass reads the card's sections (no compaction), the faults there.
  if (compacts && secs[sf::kTrkd].data.size()) TEST_ASSERT_TRUE_MESSAGE(inCompaction > 2 && flippedLanded >= 1, name);
  if (!compacts || secs[sf::kDays].data.size()) TEST_ASSERT_TRUE_MESSAGE(afterTmp > 0 && damagedAfterTmp > 0, name);
}

// The compaction's due edge's card: a base of 6,000 rows, a TRKD of 1,300,
// and 95 records (30 of TRKD's tracks, 65 new); `more` new ones beside.
void dueEdgeCard(Bytes* bin, Bytes* base, std::vector<pl::Slot>* slots, std::vector<uint64_t>* baseHashes,
                 std::vector<uint64_t>* trkdHashes, Bytes* trkd, sf::Head* head) {
  for (uint32_t i = 0; i < 6000; ++i) baseHashes->push_back(hashOf(i));
  for (uint32_t i = 0; i < 1300; ++i) trkdHashes->push_back(hashOf(100000 + i));
  std::sort(baseHashes->begin(), baseHashes->end());
  std::sort(trkdHashes->begin(), trkdHashes->end());
  Bytes baseRows;
  for (uint64_t x : *baseHashes) baseRows += trackRow(x, 3, 1000, 2);
  for (uint64_t x : *trkdHashes) *trkd += trackRow(x, 1, 2000, 3);
  *base = frameOf(1, {{sf::kTrks, cc::kSectionRequired, 24, baseRows}});
  sf::Head& h = *head;
  h.sinceDay = 100;
  h.round = 3;
  h.baseGen = 1;
  h.baseRows = 6000;
  h.baseCrc = headerCrcOf(*base);
  *bin = statsFile(4, h, markRow(kDevA, kEpochA, 2000), daysRow(300, 2, 400000), "", "", *trkd);
  uint32_t q = 2001;
  for (uint32_t i = 0; i < 30; ++i) slots->push_back(playA(q++, (*trkdHashes)[i * 7], at(305, 9, i)));
  for (uint32_t i = 0; i < 65; ++i) slots->push_back(playA(q++, hashOf(200000 + i), at(306, 9, i)));
}
}  // namespace

void test_a_failure_in_either_pass() {
  for (const char* name : {"worked-example", "compaction"}) {
    const minijson::Value& b = scenario(name)["boots"][0];
    sweep({name, cardOf(b["in"]), acksOf(b["acks"]), askOf(b), {}});
  }
  // A compaction over three streams (base 1, stats.bin's TRKD, the records).
  {
    Bytes bin, base, trkd;
    std::vector<pl::Slot> slots;
    std::vector<uint64_t> bh, th;
    sf::Head h;
    dueEdgeCard(&bin, &base, &slots, &bh, &th, &trkd, &h);
    slots.push_back(playA(5000, hashOf(300000), at(307, 9)));
    sx::SaveAsk ask;
    ask.nowMs = at(310, 12);
    ask.mayCompact = true;
    ask.playing = false;
    ask.safePoint = true;
    sweep({"three streams", {{"stats.bin", bin}, {"stats-00000001.bin", base}}, cc::msmf::Acks(), ask, slots});
  }
  // Damaged for good (a byte of MARK's flipped on the card): the counting
  // pass finds it; the next boot reads the file Corrupt.
  {
    Work w;
    const minijson::Value& b = scenario("worked-example")["boots"][0];
    const Files in = cardOf(b["in"]);
    cutfs::CutFs card;
    put(card, in);
    sf::Fold f;
    sx::Store store(card);
    TEST_ASSERT_TRUE(statsOn(store.read(f, w.p(), waitHook).read));
    Ranges rg;
    TEST_ASSERT_TRUE(rg.begin(f, acksOf(b["acks"])));
    scanLog(f, card, {});
    Bytes bad = in.at("stats.bin");
    bad[offsetOf(bad, sf::kMark)] ^= 0xFF;
    put(card, {{"stats.bin", bad}});
    const sx::SaveReport r = store.save(f, askOf(b), w.p());
    TEST_ASSERT_EQUAL_STRING("damaged", sx::savedName(r.what));
    TEST_ASSERT_EQUAL_HEX32(sf::kTypes[sf::kMark], r.section);
    sf::Fold g;
    sx::Store s2(card);
    TEST_ASSERT_EQUAL_STRING("corrupt", sx::readName(s2.read(g, w.p(), waitHook).read));
  }
}

void test_the_compaction_due_edge() {
  // A base of 6,000 rows (a quarter of it: 1,500 rows, so the 32 KB rule
  // decides), a TRKD of 1,300 rows, and 95 records: 30 plays of tracks TRKD
  // has (no new row) and 65 new: 1,365 merged rows, 32,760 bytes, not due
  // (counted beside the card's, 1,395: due, the bytes not statsref's); one
  // new track more, 1,366 rows, 32,784 bytes: due, base ⊕ TRKD ⊕ the
  // records into stats-00000002.bin, three files open at once.
  Bytes bin, base, trkd;
  std::vector<pl::Slot> slots;
  std::vector<uint64_t> baseHashes, trkdHashes;
  sf::Head h;
  dueEdgeCard(&bin, &base, &slots, &baseHashes, &trkdHashes, &trkd, &h);
  sx::SaveAsk ask;
  ask.nowMs = at(310, 12);
  ask.mayCompact = true;
  ask.playing = false;
  ask.safePoint = true;
  sx::SaveReport r = saveAgainst(bin, base, slots, ask, "1,365 merged rows");
  TEST_ASSERT_FALSE(r.compacted);
  TEST_ASSERT_EQUAL_UINT32(1365, r.rows[sf::kTrkd]);
  TEST_ASSERT_EQUAL_UINT32(2, gPeakOpen);
  slots.push_back(playA(5000, hashOf(300000), at(307, 9)));
  r = saveAgainst(bin, base, slots, ask, "1,366 merged rows");
  TEST_ASSERT_TRUE(r.compacted);
  TEST_ASSERT_EQUAL_UINT32(0, r.rows[sf::kTrkd]);
  TEST_ASSERT_EQUAL_UINT32(6000 + 1366, r.baseRows);
  TEST_ASSERT_EQUAL_UINT32(3, gPeakOpen);
  // With a base TRKD shares rows with: a 1,310-row TRKD, 10 of its hashes
  // the base's: 1,375 merged rows, due; the new base 7,365 rows (those 10
  // base ⊕ TRKD).
  Bytes shared = trkd;
  for (uint32_t i = 0; i < 10; ++i) shared += trackRow(baseHashes[i * 3], 1, 3000, 4);
  std::vector<Bytes> rows = rowsOf(shared, 24, sf::kTrkd);
  std::sort(rows.begin(), rows.end(),
            [](const Bytes& x, const Bytes& y) { return sf::compareRows(sf::kTrkd, u8(x), u8(y)) < 0; });
  const Bytes bin2 = statsFile(4, h, markRow(kDevA, kEpochA, 2000), daysRow(300, 2, 400000), "", "", join(rows));
  slots.resize(95);
  r = saveAgainst(bin2, base, slots, ask, "a TRKD sharing hashes with the base");
  TEST_ASSERT_TRUE(r.compacted);
  TEST_ASSERT_EQUAL_UINT32(6000 + 1365, r.baseRows);
}

// ---------------------------------------------------------------------------
// Compaction.
// ---------------------------------------------------------------------------
void test_compaction() {
  Work w;
  const minijson::Value& b = scenario("compaction")["boots"][0];
  const Files in = cardOf(b["in"]);
  const Files want = cardOf(b["out"]);
  // The new base can't be written: the save goes on uncompacted, as
  // compaction-playing's (the same card, a track playing).
  const Files uncompacted = cardOf(scenario("compaction-playing")["boots"][0]["out"]);
  {
    cutfs::CutFs card;
    put(card, in);
    Wrap fs(card);
    fs.failCreate = "/.player/stats-";
    const BootOut r = fixtureBoot(card, fs, b, w);
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(r.s.what));
    TEST_ASSERT_TRUE(r.s.compactFailed);
    TEST_ASSERT_FALSE(r.s.compacted);
    sameFiles(uncompacted, filesOf(card), "compaction failed");
  }
  // The old base's read fails midway: the same.
  {
    cutfs::CutFs card;
    put(card, in);
    sf::Fold f;
    sx::Store store(card);
    TEST_ASSERT_TRUE(statsOn(store.read(f, w.p(), waitHook).read));
    Ranges rg;
    TEST_ASSERT_TRUE(rg.begin(f, acksOf(b["acks"])));
    scanLog(f, card, {});
    card.faultPrefix = baseName(1);
    card.faultEvents = 0;
    card.faultFrom = 5;  // the open, the frame's four reads, then the rows
    const sx::SaveReport r = store.save(f, askOf(b), w.p());
    card.faultFrom = -1;
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(r.what));
    TEST_ASSERT_TRUE(r.compactFailed);
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == uncompacted.at("stats.bin"));
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(baseName(2)));
    TEST_ASSERT_TRUE(bytesOf(card, "stats-00000001.bin") == in.at("stats-00000001.bin"));
  }
  // The old base's rows changed on the card since the boot's read (its
  // header, which names their CRC, as it was): their CRC fails as they
  // pass, so no base is made of them; the save goes on uncompacted.
  {
    cutfs::CutFs card;
    put(card, in);
    sf::Fold f;
    sx::Store store(card);
    TEST_ASSERT_TRUE(statsOn(store.read(f, w.p(), waitHook).read));
    Ranges rg;
    TEST_ASSERT_TRUE(rg.begin(f, acksOf(b["acks"])));
    scanLog(f, card, {});
    Bytes changed = in.at("stats-00000001.bin");
    changed[changed.size() - 1] ^= 1;
    put(card, {{"stats-00000001.bin", changed}});
    const sx::SaveReport r = store.save(f, askOf(b), w.p());
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(r.what));
    TEST_ASSERT_TRUE(r.compactFailed);
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == uncompacted.at("stats.bin"));
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(baseName(2)));
  }
  // A stale stats-00000002.bin (a cut compaction's): the read removes it,
  // a neighbour of the base checked whole, and the compaction writes the
  // new base under its name. (Over a stale file the read leaves, a lost
  // base's, FA_CREATE_ALWAYS replaces it: the base-other and corrupt
  // fixtures.) Nothing freed that another entry holds.
  {
    cutfs::CutFs card;
    put(card, in);
    put(card, {{"stats-00000002.bin", Bytes(5000, 'z')}});
    const BootOut r = fixtureBoot(card, card, b, w);
    TEST_ASSERT_EQUAL_UINT32(1, r.o.removed);
    TEST_ASSERT_TRUE(r.s.compacted);
    TEST_ASSERT_EQUAL_UINT32(0, card.violations.size());
    sameFiles(want, filesOf(card), "a stale base under the new name");
  }
  // Never under the name the card's stats.bin names: base-other's first
  // boot (a lost base 1, its next 1 again) skips it; the next boot's does.
  {
    const minijson::Value& o = scenario("base-other")["boots"][0];
    cutfs::CutFs card;
    put(card, cardOf(o["in"]));
    const BootOut r = fixtureBoot(card, card, o, w);
    TEST_ASSERT_TRUE(r.o.baseLost);
    TEST_ASSERT_TRUE(r.s.compactSkipped);
    TEST_ASSERT_FALSE(r.s.compacted);
    sameFiles(cardOf(o["out"]), filesOf(card), "base-other's first boot");
  }
  // A compaction held (its replace() failed): RAM's HEAD names the new
  // base; the save that promotes it merges the newer record onto it (TRKD
  // that row alone), never compacting over the name stats.bin then names;
  // the old base goes at the next boot (a neighbour named by nothing).
  {
    cutfs::CutFs card;
    put(card, in);
    Wrap fs(card);
    fs.failTmpRenames = 1;
    sf::Fold f;
    sx::Store store(fs);
    const BootOut r = bootOnce(card, store, f, w.p(), acksOf(b["acks"]), {}, askOf(b));
    TEST_ASSERT_EQUAL_STRING("held", sx::savedName(r.s.what));
    TEST_ASSERT_TRUE(r.s.compacted);
    TEST_ASSERT_EQUAL_UINT32(2, f.head().baseGen);
    const Bytes base2 = bytesOf(card, "stats-00000002.bin");
    TEST_ASSERT_TRUE(base2 == want.at("stats-00000002.bin"));
    pl::Slot s;
    TEST_ASSERT_TRUE(pl::decode(u8(in.at("plays.jnl")), pl::kSlot, &s) == pl::Verdict::Ok);
    s.seq = 17;
    f.liveSlot(s);
    const sx::SaveReport q = store.save(f, askOf(b), w.p());
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(q.what));
    TEST_ASSERT_TRUE(q.heldPromoted);
    TEST_ASSERT_FALSE(q.compacted);
    TEST_ASSERT_EQUAL_UINT32(1, q.rows[sf::kTrkd]);
    TEST_ASSERT_TRUE(bytesOf(card, "stats-00000002.bin") == base2);
    TEST_ASSERT_EQUAL_UINT32(2, f.head().baseGen);
    TEST_ASSERT_TRUE(bytesOf(card, "stats-00000001.bin") == in.at("stats-00000001.bin"));
    // The next boot reads it whole, and the orphan goes.
    sf::Fold g;
    sx::Store s2(card);
    const sx::Opened o = s2.read(g, w.p(), waitHook);
    TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o.read));
    TEST_ASSERT_EQUAL_UINT32(1, o.removed);
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(baseName(1)));
    TEST_ASSERT_EQUAL_UINT32(0, card.violations.size());
  }
}

// ---------------------------------------------------------------------------
// The writer in the caller's memory.
// ---------------------------------------------------------------------------
void test_the_writer_in_the_callers_memory() {
  // A big stats.bin: a full log's fold (13,106 slots, 5,000 tracks), the
  // boot's overflow in 16 chunks.
  Work w;
  cutfs::CutFs card;
  Wrap fs(card);
  sf::Fold f(heapAlloc, heapFree);
  sx::Store store(fs);
  TEST_ASSERT_EQUAL_STRING("absent", sx::readName(store.read(f, w.p(), waitHook).read));
  cc::msmf::Acks none;
  Ranges rg;
  TEST_ASSERT_TRUE(rg.begin(f, none));
  uint64_t x = 2026;
  for (uint32_t q = 1; q <= 2 * pl::kFileSlots; ++q) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    pl::Slot s;
    s.kind = pl::kKindPlay;
    s.outcome = pl::kCompleted;
    s.source = static_cast<uint8_t>(x % 4);
    s.clock = pl::kClockRtc;
    s.seq = q;
    s.epoch = kEpochA;
    std::memcpy(s.deviceId, kDevA, 6);
    s.startedAt = sf::kMinMs + static_cast<uint64_t>(q / 25) * sf::kDayMs + (q % 25) * sf::kHourMs / 2;
    s.bootSeq = 1;
    s.playedMs = 30000 + static_cast<uint32_t>(x % 200000);
    s.durationMs = 200000;
    const uint32_t track = static_cast<uint32_t>((x >> 20) % 5000);
    s.pathHash = cc::fnv1a64(&track, sizeof(track));
    if ((q - 1) % 128 == 0) TEST_ASSERT_TRUE(f.reserve(128));
    f.bootSlot(s, 0);
  }
  f.endBoot();
  TEST_ASSERT_EQUAL_UINT32(2 * pl::kFileSlots, f.records());
  TEST_ASSERT_EQUAL_UINT32(sf::kMaxChunks, f.chunks());
  const size_t carves = gHeap.carves;
  sx::SaveAsk ask;
  ask.nowMs = sf::kMinMs + 530ull * sf::kDayMs;
  ask.mayCompact = true;
  ask.playing = true;  // (no compaction: the tmp's writes alone)
  ask.safePoint = false;
  card.bytesRead = 0;
  const sx::SaveReport r = store.save(f, ask, w.p());
  TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(r.what));
  TEST_ASSERT_FALSE(r.compacted);
  TEST_ASSERT_EQUAL_UINT32(carves, gHeap.carves);  // nothing carved by the save
  TEST_ASSERT_EQUAL_UINT32(0, f.chunks());         // the overflow given back once it landed
  TEST_ASSERT_EQUAL_UINT32(0, gHeap.blocks.size());
  // The tmp's writes: none crosses a 4 KB boundary of the file, and the
  // rows go in whole 4 KB pieces.
  uint32_t tmpWrites = 0, whole = 0;
  for (const Wrap::Write& wr : fs.writes) {
    if (wr.path != sx::kStatsTmp) continue;
    ++tmpWrites;
    TEST_ASSERT_TRUE(wr.n <= 4096);
    TEST_ASSERT_TRUE(wr.offset % 4096 + wr.n <= 4096);
    whole += wr.n == 4096 && wr.offset % 4096 == 0;
  }
  printf("a full log's stats.bin: %u bytes in %u writes (%u whole 4 KB pieces); rows: HOUR %u, DAYS %u, RCNT %u, "
         "TRKD %u\n",
         static_cast<unsigned>(r.bytes), static_cast<unsigned>(tmpWrites), static_cast<unsigned>(whole),
         static_cast<unsigned>(r.rows[sf::kHour]), static_cast<unsigned>(r.rows[sf::kDays]),
         static_cast<unsigned>(r.rows[sf::kRcnt]), static_cast<unsigned>(r.rows[sf::kTrkd]));
  TEST_ASSERT_TRUE(r.bytes > 100000);
  TEST_ASSERT_TRUE(tmpWrites <= r.bytes / 4096 + 6);
  TEST_ASSERT_TRUE(whole >= r.bytes / 4096 - 2);
  TEST_ASSERT_TRUE(card.peakOpen <= 2);
  // Read back: the same stats.
  sf::Fold g;
  sx::Store s2(card);
  const sx::Opened o = s2.read(g, w.p(), waitHook);
  TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o.read));
  TEST_ASSERT_EQUAL_UINT64(f.head().plays, g.head().plays);
  const std::vector<Sec> secs = sectionsOf(bytesOf(card, "stats.bin"), sx::kStatsSpec);
  TEST_ASSERT_EQUAL_UINT32(r.rows[sf::kTrkd], secs[sf::kTrkd].data.size() / 24);
  TEST_ASSERT_TRUE(r.rows[sf::kTrkd] > 4500);
  // And the next save, with a live slot: a compaction of it (no base yet:
  // TRKD ⊕ the record, stats.bin and the new base open), then read back
  // whole.
  pl::Slot s;
  s.kind = pl::kKindPlay;
  s.outcome = pl::kCompleted;
  s.seq = 2 * pl::kFileSlots + 1;
  s.epoch = kEpochA;
  std::memcpy(s.deviceId, kDevA, 6);
  s.startedAt = ask.nowMs;
  s.clock = pl::kClockRtc;
  s.playedMs = s.durationMs = 200000;
  s.pathHash = 77;
  TEST_ASSERT_TRUE(g.liveSlot(s));
  ask.playing = false;
  card.peakOpen = card.openNow;
  card.bytesRead = 0;
  const sx::SaveReport c = s2.save(g, ask, w.p());
  TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(c.what));
  TEST_ASSERT_TRUE(c.compacted);
  TEST_ASSERT_EQUAL_UINT32(r.rows[sf::kTrkd] + 1, c.baseRows);
  TEST_ASSERT_EQUAL_UINT32(2, card.peakOpen);  // (no old base: stats.bin and the new one)
  printf("its next save: %llu bytes read (stats.bin %u: the counting pass, the compaction's TRKD, the writing pass)\n",
         static_cast<unsigned long long>(card.bytesRead), static_cast<unsigned>(r.bytes));
  sf::Fold h;
  sx::Store s3(card);
  const sx::Opened o3 = s3.read(h, w.p(), waitHook);
  TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o3.read));
  TEST_ASSERT_EQUAL_UINT32(c.baseRows, h.head().baseRows);
  TEST_ASSERT_EQUAL_UINT32(0, sectionsOf(bytesOf(card, "stats.bin"), sx::kStatsSpec)[sf::kTrkd].data.size());
}

int main(int, char**) {
  if (!cardfixtures::json("stats/index.json", &gIndex) || gIndex.size() == 0 ||
      !cardfixtures::json("stats/sessions.json", &gSessions) || gSessions.size() == 0) {
    printf("the stats fixtures (test/fixtures/card/stats) can't be read\n");
    return 1;
  }
  UNITY_BEGIN();
  RUN_TEST(test_the_names_and_the_frame);
  RUN_TEST(test_stats_check);
  RUN_TEST(test_a_lone_major_2_tmp_survives_settle);
  RUN_TEST(test_every_fixture_boot);
  RUN_TEST(test_every_session_the_store_runs);
  RUN_TEST(test_held_then_promoted_as_if_landed_once);
  RUN_TEST(test_the_boot_reads_again);
  RUN_TEST(test_marks_block);
  RUN_TEST(test_stats_bad_never_shares_a_chain);
  RUN_TEST(test_the_base_check);
  RUN_TEST(test_the_card_copy_is_checked_first);
  RUN_TEST(test_the_commit_steps);
  RUN_TEST(test_a_fold_out_of_memory_is_never_saved);
  RUN_TEST(test_a_cut_at_every_step_of_a_save);
  RUN_TEST(test_the_cuts_at_their_edges);
  RUN_TEST(test_the_compaction_due_edge);
  RUN_TEST(test_a_failure_in_either_pass);
  RUN_TEST(test_compaction);
  RUN_TEST(test_the_writer_in_the_callers_memory);
  return UNITY_END();
}
