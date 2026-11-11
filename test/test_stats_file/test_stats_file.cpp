// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the listening stats' files (lib/core StatsFile;
// docs/LISTENING-STATS.md 5.2-5.5, 5.7, 5.12), on a card in memory that can
// lose its power at any step (test/support/CutFs.h), against
// tools/statsref.py's fixtures (test/fixtures/card/stats/index.json):
// - the names, and a base's frame as ContainerWriter lays it out;
// - StatsCheck: Whole for a whole frame and for Major or UnknownRequired,
//   Unknown for a read the card fails, damaged records or no memory, Torn
//   only for a frame that fails its own checks; a lone major-2 stats.tmp
//   survives settle() (promoted, then read as Newer and left alone);
// - every boot of every fixture: the read's verdict and Why, the fold, the
//   save, a compaction, the trim by the card's MARK, and the card after it
//   file for file, byte for byte (stats.bin, the bases, stats.bad, the
//   neighbours removed, the log files trimmed);
// - the boot's reads again through the wait hook (100, 300, 1,000 ms) and
//   a remount's one; a corrupt file read twice, then stats.bad (an older one
//   removed first; never onto a chain a twin holds); damaged records read
//   once;
// - the base: lost only when the card says it isn't there or its header's
//   CRC is another's; an open or a read the card fails, a base shorter than
//   its header, rows out of order or of another count: Unread, nothing
//   removed;
// - the commit: nothing new writes nothing; a held tmp promoted first (its
//   header checked), the generation after it; the card's copy checked
//   first (a change refuses every later save); a full card, a size that
//   doesn't match; a cut at every step of a save (with and without a
//   compaction, three ways), then a boot: the card as the uncut boot left
//   it, no chain freed twice;
// - compaction: base ⊕ delta, a failed one (the save goes on), never over
//   the base the card's stats.bin names;
// - the writer in the caller's memory: no carve during a save, writes in
//   pieces that end on the file's 4 KB boundaries.
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
using Bytes = std::string;
using Files = std::map<std::string, Bytes>;
const std::string kDir = "/.player/";

// Made-up devices (RFC 7042's documentation MACs) and epochs.
const uint8_t kDevA[6] = {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01};
constexpr uint64_t kEpochA = 0x1122334455667788ull;

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

// A heap that counts the Fold's carves.
struct Heap {
  std::map<void*, size_t> blocks;
  size_t carves = 0;
};
Heap gHeap;
void* heapAlloc(size_t n) {
  void* p = std::malloc(n);
  gHeap.blocks[p] = n;
  ++gHeap.carves;
  return p;
}
void heapFree(void* p) {
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

uint32_t generationOf(const Bytes& b) { return b.size() >= 24 ? cc::get32(reinterpret_cast<const uint8_t*>(b.data()) + 20) : 0; }
uint32_t headerCrcOf(const Bytes& b) { return b.size() >= 36 ? cc::get32(reinterpret_cast<const uint8_t*>(b.data()) + 32) : 0; }

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

const minijson::Value& scenario(const char* name) {
  for (size_t i = 0; i < gIndex.size(); ++i)
    if (gIndex[i]["name"].str() == name) return gIndex[i];
  TEST_FAIL_MESSAGE(name);
  return gIndex[0];
}

void sameFiles(const Files& want, const Files& got, const char* what) {
  std::string names;
  for (const auto& f : want)
    if (!got.count(f.first)) names += " -" + f.first;
  for (const auto& f : got)
    if (!want.count(f.first)) names += " +" + f.first;
  TEST_ASSERT_TRUE_MESSAGE(names.empty(), (std::string(what) + ": the card's names differ:" + names).c_str());
  for (const auto& f : want) {
    const Bytes& g = got.at(f.first);
    TEST_ASSERT_TRUE_MESSAGE(g == f.second, (std::string(what) + ": " + f.first + ", " + hex(g, f.second)).c_str());
  }
}

// ---------------------------------------------------------------------------
// The card through a hand that can fail one kind of step, and that
// records the writes.
// ---------------------------------------------------------------------------
class Wrap : public tagstore::Fs {
public:
  explicit Wrap(cutfs::CutFs& c) : c_(c) {}
  std::string failCreate;   // an open to create a path with this prefix fails
  int failTmpRenames = 0;   // so many renames of stats.tmp to stats.bin fail
  int failBinRemoves = 0;   // so many removes of stats.bin fail
  int sizeOff = 0;          // stats.tmp's size, as a read's open has it, off by this
  struct Write {
    std::string path;
    uint32_t offset, n;
  };
  std::vector<Write> writes;

  class F : public tagstore::File {
  public:
    F(Wrap* w, tagstore::File* in, const std::string& path, int off) : w_(w), in_(in), path_(path), off_(off) {}
    uint32_t size() const override { return static_cast<uint32_t>(static_cast<int64_t>(in_->size()) + off_); }
    bool read(uint32_t offset, void* out, uint32_t n) override { return in_->read(offset, out, n); }
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
// log in the scan's order, the boot's save, the trim by the card's MARK.
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

void scanLog(sf::Fold& f, const cutfs::CutFs& card, const std::map<std::string, int>& unread) {
  for (int i = 0; i < sf::kFiles; ++i) {
    if (!card.dir.count(kDir + kLogNames[i])) continue;
    const Bytes b = bytesOf(card, kLogNames[i]);
    uint32_t slots = static_cast<uint32_t>(std::min<size_t>(b.size(), pl::kFileBytes) / pl::kSlot);
    auto u = unread.find(kLogNames[i]);
    if (u != unread.end()) slots = u->second < 0 ? 0 : std::min<uint32_t>(slots, static_cast<uint32_t>(u->second));
    for (uint32_t k = 0; k < slots; ++k) {
      pl::Slot s;
      if (pl::decode(reinterpret_cast<const uint8_t*>(b.data()) + k * pl::kSlot, pl::kSlot, &s) == pl::Verdict::Ok)
        f.bootSlot(s, i);
    }
    if (u != unread.end()) f.partial();
  }
}

// 2.20.2's trim as the journal judges a file, and 5.5's: every play slot
// at or under the card's MARK.
bool trimmable(sf::Fold& f, const Bytes& b, const cc::msmf::Acks& acks) {
  if (b.size() > pl::kFileBytes) return false;
  bool anyPlay = false, allAcked = true, lastIsPlay = false, covered = true;
  for (size_t k = 0; k + pl::kSlot <= b.size(); k += pl::kSlot) {
    pl::Slot s;
    const pl::Verdict v = pl::decode(reinterpret_cast<const uint8_t*>(b.data()) + k, pl::kSlot, &s);
    if (v == pl::Verdict::Newer || v == pl::Verdict::Kind) return false;
    if (v != pl::Verdict::Ok) continue;
    lastIsPlay = s.kind == pl::kKindPlay;
    if (s.kind != pl::kKindPlay) continue;
    anyPlay = true;
    if (!pl::acknowledged(s, &acks)) allAcked = false;
    if (!f.covered(s)) covered = false;
  }
  return anyPlay && allAcked && lastIsPlay && covered;
}

struct BootOut {
  sx::Opened o;
  sx::SaveReport s;
  std::set<std::string> trimmed;
};

BootOut bootOnce(tagstore::Fs& fs, cutfs::CutFs& card, sx::Store& store, sf::Fold& f, uint8_t* work,
                 const cc::msmf::Acks& acks, const std::map<std::string, int>& unread, const sx::SaveAsk& ask,
                 bool saveFails = false, sx::Store::WaitFn wait = waitHook) {
  (void)fs;
  BootOut out;
  out.o = store.read(f, work, wait);
  if (!sx::statsOn(out.o.read)) return out;
  TEST_ASSERT_TRUE(f.beginBoot(acks));
  TEST_ASSERT_TRUE(f.reserve(128));
  scanLog(f, card, unread);
  f.endBoot();
  card.refuse = saveFails;
  out.s = store.save(f, ask, work);
  card.refuse = false;
  if (card.dead || acks.count == 0) return out;
  // The trim: plays.1.jnl to plays.9.jnl, then plays.jnl.
  for (int k = 1; k <= sf::kFiles; ++k) {
    const char* n = kLogNames[k % sf::kFiles];
    if (!card.dir.count(kDir + n) || unread.count(n)) continue;
    if (trimmable(f, bytesOf(card, n), acks) && card.remove((kDir + n).c_str())) out.trimmed.insert(n);
  }
  return out;
}

// One boot of a fixture on `card` (its "in" put there by the caller).
BootOut fixtureBoot(cutfs::CutFs& card, tagstore::Fs& fs, const minijson::Value& b, Work& w,
                    sx::Store::WaitFn wait = waitHook) {
  sf::Fold f;
  TEST_ASSERT_TRUE(f.begin());
  sx::Store store(fs);
  return bootOnce(fs, card, store, f, w.p(), acksOf(b["acks"]), unreadOf(b), askOf(b), b["saveFails"].boolean(), wait);
}

// stats.bin's bytes with another major (its header's CRC made again).
Bytes resealed(Bytes b, uint16_t major) {
  uint8_t* p = reinterpret_cast<uint8_t*>(&b[0]);
  cc::put16(p + 4, major);
  const uint32_t hb = cc::get32(p + 8), n = cc::get32(p + 12);
  cc::put32(p + 32, 0);
  cc::put32(p + 32, cc::crc32(p, hb + 32 * n));
  return b;
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
  // The caller's memory: the writer, the plan, a frame, then two 4 KB buffers.
  TEST_ASSERT_TRUE(sizeof(sx::Scratch) <= sx::kScratchBytes);
  TEST_ASSERT_EQUAL_UINT32(sx::kScratchBytes + 8192, sx::kWorkBytes);
  printf("the work memory: %u bytes (the writer %u, the rest of the scratch %u)\n",
         static_cast<unsigned>(sx::kWorkBytes), static_cast<unsigned>(sizeof(cc::ContainerWriter)),
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
    TEST_ASSERT_TRUE(f.begin());
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
    TEST_ASSERT_TRUE(f.begin());
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
  int boots = 0, saves = 0, compactions = 0, trims = 0, removed = 0, bad = 0;
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
    }
  }
  printf("fixtures: %d boots, %d saves and %d bases byte for byte, %d removed, %d stats.bad, %d files trimmed\n",
         boots, saves, compactions, removed, bad, trims);
  TEST_ASSERT_EQUAL_INT(34, boots);
  TEST_ASSERT_EQUAL_INT(25, saves);
  TEST_ASSERT_EQUAL_INT(4, compactions);
  TEST_ASSERT_EQUAL_INT(2, removed);
  TEST_ASSERT_EQUAL_INT(1, bad);
  TEST_ASSERT_EQUAL_INT(22, trims);
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
    TEST_ASSERT_TRUE(f.begin());
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
    TEST_ASSERT_TRUE(f.begin());
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
    TEST_ASSERT_TRUE(f.begin());
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
    TEST_ASSERT_TRUE(f.begin());
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
    TEST_ASSERT_TRUE(f.begin());
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
  // No work memory: no read.
  {
    cutfs::CutFs card;
    put(card, in);
    sf::Fold f;
    TEST_ASSERT_TRUE(f.begin());
    sx::Store store(card);
    TEST_ASSERT_EQUAL_STRING("noMemory", sx::readName(store.read(f, nullptr, waitHook).read));
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
      TEST_ASSERT_TRUE(f.begin());
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
    TEST_ASSERT_TRUE(f.begin());
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
    TEST_ASSERT_TRUE(f.begin());
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
  sf::Head h = sf::decodeHead(reinterpret_cast<const uint8_t*>(secs[0].data.data()));
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
    TEST_ASSERT_TRUE(f.begin());
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
    TEST_ASSERT_TRUE(f.begin());
    sx::Store store(card);
    const sx::Opened o = store.read(f, w.p(), waitHook);
    TEST_ASSERT_TRUE(statsOn(o.read));
    TEST_ASSERT_TRUE(f.beginBoot(acksOf(b["acks"])));
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
    TEST_ASSERT_TRUE(f.begin());
    sx::Store store(card);
    const BootOut r = bootOnce(card, card, store, f, w.p(), acksOf(b2["acks"]), {}, askOf(b2));
    TEST_ASSERT_EQUAL_STRING("nothing new", sx::savedName(r.s.what));
    TEST_ASSERT_TRUE(sx::savedOk(r.s.what));
    TEST_ASSERT_EQUAL_INT32(before, card.steps);
  }
  // No work memory.
  {
    cutfs::CutFs card;
    put(card, in);
    sf::Fold f;
    TEST_ASSERT_TRUE(f.begin());
    sx::Store store(card);
    TEST_ASSERT_TRUE(statsOn(store.read(f, w.p(), waitHook).read));
    TEST_ASSERT_TRUE(f.beginBoot(acksOf(b["acks"])));
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
    TEST_ASSERT_TRUE(f.begin());
    sx::Store store(fs);
    const BootOut r = bootOnce(fs, card, store, f, w.p(), acksOf(b["acks"]), {}, askOf(b));
    TEST_ASSERT_EQUAL_STRING("failed", sx::savedName(r.s.what));
    TEST_ASSERT_EQUAL_STRING("size", r.s.step);
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsTmp));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == in.at("stats.bin"));
    TEST_ASSERT_TRUE(f.dirty());
  }
  // replace() fails at its rename, stats.bin removed: the tmp held, whole.
  // A reboot's settle() promotes it (stats.bin gone). In the session, the
  // next save promotes it first, then writes the generation after it.
  {
    cutfs::CutFs card;
    put(card, in);
    Wrap fs(card);
    fs.failTmpRenames = 1;
    sf::Fold f;
    TEST_ASSERT_TRUE(f.begin());
    sx::Store store(fs);
    const BootOut r = bootOnce(fs, card, store, f, w.p(), acksOf(b["acks"]), {}, askOf(b));
    TEST_ASSERT_EQUAL_STRING("held", sx::savedName(r.s.what));
    TEST_ASSERT_TRUE(store.held());
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsBin));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.tmp") == want.at("stats.bin"));
    // (The trim followed the MARK read at the boot: plays.1.jnl kept.)
    TEST_ASSERT_EQUAL_UINT32(0, r.trimmed.size());
    {
      cutfs::CutFs next = card.reboot(cutfs::Variant::LoseUnsynced);
      sf::Fold g;
      TEST_ASSERT_TRUE(g.begin());
      sx::Store s2(next);
      const sx::Opened o = s2.read(g, w.p(), waitHook);
      TEST_ASSERT_TRUE(o.settled.what == tagstore::Settle::Promoted);
      TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o.read));
      TEST_ASSERT_TRUE(bytesOf(next, "stats.bin") == want.at("stats.bin"));
    }
    // A live play, then the next save (a rotation's).
    pl::Slot s;
    TEST_ASSERT_TRUE(pl::decode(reinterpret_cast<const uint8_t*>(bytesOf(card, "plays.jnl").data()), pl::kSlot, &s) ==
                     pl::Verdict::Ok);
    s.seq = 49;
    f.liveSlot(s);
    sx::SaveAsk ask = askOf(b);
    ask.mayCompact = false;
    const sx::SaveReport q = store.save(f, ask, w.p());
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(q.what));
    TEST_ASSERT_TRUE(q.heldPromoted);
    TEST_ASSERT_FALSE(store.held());
    TEST_ASSERT_EQUAL_UINT32(generationOf(want.at("stats.bin")) + 1, q.generation);
    TEST_ASSERT_EQUAL_UINT32(q.generation, generationOf(bytesOf(card, "stats.bin")));
    TEST_ASSERT_EQUAL_UINT32(q.generation, f.generation());
    TEST_ASSERT_EQUAL_UINT32(49, f.markOf(s.deviceId, s.epoch));
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsTmp));
    TEST_ASSERT_EQUAL_UINT32(0, card.violations.size());
  }
  // replace() fails at its remove: stats.bin as it was, the tmp held. A
  // remount's read promotes it first (settle() would have dropped it).
  {
    cutfs::CutFs card;
    put(card, in);
    Wrap fs(card);
    fs.failBinRemoves = 1;
    sf::Fold f;
    TEST_ASSERT_TRUE(f.begin());
    sx::Store store(fs);
    const BootOut r = bootOnce(fs, card, store, f, w.p(), acksOf(b["acks"]), {}, askOf(b));
    TEST_ASSERT_EQUAL_STRING("held", sx::savedName(r.s.what));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == in.at("stats.bin"));
    const sx::Opened o = store.read(f, w.p(), nullptr);
    TEST_ASSERT_TRUE(o.heldPromoted);
    TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o.read));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == want.at("stats.bin"));
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsTmp));
    TEST_ASSERT_FALSE(store.held());
  }
  // A held tmp that is no longer ours (another player's tmp under its
  // name): left to settle(), which drops it while stats.bin is there; the
  // card's copy is the one RAM stands on, so the save goes on.
  {
    cutfs::CutFs card;
    put(card, in);
    Wrap fs(card);
    fs.failBinRemoves = 1;
    sf::Fold f;
    TEST_ASSERT_TRUE(f.begin());
    sx::Store store(fs);
    bootOnce(fs, card, store, f, w.p(), acksOf(b["acks"]), {}, askOf(b));
    TEST_ASSERT_TRUE(store.held());
    const Bytes theirs = cardOf(scenario("two-devices")["boots"][0]["out"]).at("stats.bin");
    card.put(sx::kStatsTmp, std::vector<uint8_t>(theirs.begin(), theirs.end()));
    const sx::SaveReport q = store.save(f, askOf(b), w.p());
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(q.what));
    TEST_ASSERT_FALSE(q.heldPromoted);
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == want.at("stats.bin"));
  }
  // A full card: every step fails, the card as it was.
  {
    cutfs::CutFs card;
    put(card, in);
    sf::Fold f;
    TEST_ASSERT_TRUE(f.begin());
    sx::Store store(card);
    const BootOut r = bootOnce(card, card, store, f, w.p(), acksOf(b["acks"]), {}, askOf(b), true);
    TEST_ASSERT_EQUAL_STRING("failed", sx::savedName(r.s.what));
    TEST_ASSERT_EQUAL_STRING("tmp", r.s.step);
    TEST_ASSERT_FALSE(store.held());
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == in.at("stats.bin"));
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsTmp));
  }
  // A partial scan: no save (Off).
  {
    const minijson::Value& p = scenario("partial-open")["boots"][0];
    cutfs::CutFs card;
    put(card, cardOf(p["in"]));
    sf::Fold f;
    TEST_ASSERT_TRUE(f.begin());
    sx::Store store(card);
    const BootOut r = bootOnce(card, card, store, f, w.p(), acksOf(p["acks"]), unreadOf(p), askOf(p));
    TEST_ASSERT_EQUAL_STRING("off", sx::savedName(r.s.what));
    TEST_ASSERT_FALSE(sx::savedOk(r.s.what));
  }
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
        TEST_ASSERT_TRUE(f.begin());
        sx::Store store(card);
        const sx::Opened o = store.read(f, w.p(), waitHook);
        TEST_ASSERT_TRUE(statsOn(o.read));
        TEST_ASSERT_TRUE(f.beginBoot(acksOf(b["acks"])));
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
    TEST_ASSERT_TRUE(f.begin());
    sx::Store store(card);
    TEST_ASSERT_TRUE(statsOn(store.read(f, w.p(), waitHook).read));
    TEST_ASSERT_TRUE(f.beginBoot(acksOf(b["acks"])));
    scanLog(f, card, {});
    card.faultPrefix = baseName(1);
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
    TEST_ASSERT_TRUE(f.begin());
    sx::Store store(card);
    TEST_ASSERT_TRUE(statsOn(store.read(f, w.p(), waitHook).read));
    TEST_ASSERT_TRUE(f.beginBoot(acksOf(b["acks"])));
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
  // A compaction held (its replace() failed), promoted by the next save:
  // that save never compacts over the base the card's stats.bin now names.
  {
    cutfs::CutFs card;
    put(card, in);
    Wrap fs(card);
    fs.failTmpRenames = 1;
    sf::Fold f;
    TEST_ASSERT_TRUE(f.begin());
    sx::Store store(fs);
    const BootOut r = bootOnce(fs, card, store, f, w.p(), acksOf(b["acks"]), {}, askOf(b));
    TEST_ASSERT_EQUAL_STRING("held", sx::savedName(r.s.what));
    TEST_ASSERT_TRUE(r.s.compacted);
    const Bytes base2 = bytesOf(card, "stats-00000002.bin");
    TEST_ASSERT_TRUE(base2 == want.at("stats-00000002.bin"));
    pl::Slot s;
    TEST_ASSERT_TRUE(pl::decode(reinterpret_cast<const uint8_t*>(in.at("plays.jnl").data()), pl::kSlot, &s) ==
                     pl::Verdict::Ok);
    s.seq = 17;
    f.liveSlot(s);
    const sx::SaveReport q = store.save(f, askOf(b), w.p());
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(q.what));
    TEST_ASSERT_TRUE(q.heldPromoted);
    TEST_ASSERT_TRUE(q.compactSkipped);
    TEST_ASSERT_FALSE(q.compacted);
    TEST_ASSERT_TRUE(bytesOf(card, "stats-00000002.bin") == base2);
    TEST_ASSERT_EQUAL_UINT32(1, f.head().baseGen);  // RAM's: the old base, still there
    TEST_ASSERT_TRUE(bytesOf(card, "stats-00000001.bin") == in.at("stats-00000001.bin"));
    // The next boot reads it whole, and the orphan goes.
    sf::Fold g;
    TEST_ASSERT_TRUE(g.begin());
    sx::Store s2(card);
    const sx::Opened o = s2.read(g, w.p(), waitHook);
    TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o.read));
    TEST_ASSERT_EQUAL_UINT32(1, o.removed);
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(baseName(2)));
    TEST_ASSERT_EQUAL_UINT32(0, card.violations.size());
  }
}

// ---------------------------------------------------------------------------
// The writer in the caller's memory.
// ---------------------------------------------------------------------------
void test_the_writer_in_the_callers_memory() {
  // A big stats.bin: a full log's fold (13,106 slots, 5,000 tracks).
  Work w;
  cutfs::CutFs card;
  Wrap fs(card);
  sf::Fold f(heapAlloc, heapFree);
  TEST_ASSERT_TRUE(f.begin());
  sx::Store store(fs);
  TEST_ASSERT_EQUAL_STRING("absent", sx::readName(store.read(f, w.p(), waitHook).read));
  cc::msmf::Acks none;
  TEST_ASSERT_TRUE(f.beginBoot(none));
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
  const size_t carves = gHeap.carves;
  sx::SaveAsk ask;
  ask.nowMs = sf::kMinMs + 530ull * sf::kDayMs;
  ask.mayCompact = true;
  ask.playing = true;  // (no compaction: the tmp's writes alone)
  ask.safePoint = false;
  const sx::SaveReport r = store.save(f, ask, w.p());
  TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(r.what));
  TEST_ASSERT_FALSE(r.compacted);
  TEST_ASSERT_EQUAL_UINT32(carves, gHeap.carves);  // nothing carved by the save
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
  printf("a full log's stats.bin: %u bytes in %u writes (%u whole 4 KB pieces)\n", static_cast<unsigned>(r.bytes),
         static_cast<unsigned>(tmpWrites), static_cast<unsigned>(whole));
  TEST_ASSERT_TRUE(r.bytes > 100000);
  TEST_ASSERT_TRUE(tmpWrites <= r.bytes / 4096 + 6);
  TEST_ASSERT_TRUE(whole >= r.bytes / 4096 - 2);
  // Read back: the same stats.
  sf::Fold g;
  TEST_ASSERT_TRUE(g.begin());
  sx::Store s2(card);
  const sx::Opened o = s2.read(g, w.p(), waitHook);
  TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o.read));
  TEST_ASSERT_EQUAL_UINT32(f.rows(sf::kTrkd), g.rows(sf::kTrkd));
  TEST_ASSERT_EQUAL_UINT32(f.rows(sf::kRcnt), g.rows(sf::kRcnt));
  TEST_ASSERT_EQUAL_UINT64(f.head().plays, g.head().plays);
  // And a compaction of it: base ⊕ delta, then read back whole.
  g.liveSlot([&] {
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
    return s;
  }());
  ask.playing = false;
  const sx::SaveReport c = s2.save(g, ask, w.p());
  TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(c.what));
  TEST_ASSERT_TRUE(c.compacted);
  TEST_ASSERT_EQUAL_UINT32(f.rows(sf::kTrkd) + 1, c.baseRows);
  sf::Fold h;
  TEST_ASSERT_TRUE(h.begin());
  sx::Store s3(card);
  const sx::Opened o3 = s3.read(h, w.p(), waitHook);
  TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(o3.read));
  TEST_ASSERT_EQUAL_UINT32(0, h.rows(sf::kTrkd));
  TEST_ASSERT_EQUAL_UINT32(c.baseRows, h.head().baseRows);
}

int main(int, char**) {
  if (!cardfixtures::json("stats/index.json", &gIndex) || gIndex.size() == 0) {
    printf("the stats fixtures (test/fixtures/card/stats) can't be read\n");
    return 1;
  }
  UNITY_BEGIN();
  RUN_TEST(test_the_names_and_the_frame);
  RUN_TEST(test_stats_check);
  RUN_TEST(test_a_lone_major_2_tmp_survives_settle);
  RUN_TEST(test_every_fixture_boot);
  RUN_TEST(test_the_boot_reads_again);
  RUN_TEST(test_stats_bad_never_shares_a_chain);
  RUN_TEST(test_the_base_check);
  RUN_TEST(test_the_card_copy_is_checked_first);
  RUN_TEST(test_the_commit_steps);
  RUN_TEST(test_a_cut_at_every_step_of_a_save);
  RUN_TEST(test_compaction);
  RUN_TEST(test_the_writer_in_the_callers_memory);
  return UNITY_END();
}
