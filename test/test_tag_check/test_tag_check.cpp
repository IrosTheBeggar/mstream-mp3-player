// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the console's gs a piece a loop pass (lib/core TagCheck,
// mptg::Walker::setEndPiece(), tagstore::RowTally): the same answers as the
// one call gs made before (app/TagConsole's find() and status()'s rows:
// openDevice(), the walk with HIDX, File::open(), BuilderRows) on D and T,
// valid and broken; every step's reads bounded, at N11's 19,412 records,
// where that one call read the whole of D (it held the loop 12.4 s on the
// device, 2026-10-14); and gs's wait for the worker's step under way
// (tagcheck::waitOver()). Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../support/SynthCard.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "CardTags.h"
#include "LibraryBuilder.h"
#include "LibrarySynth.h"
#include "TagCheck.h"
#include "TagStore.h"

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;
namespace ts = tagstore;
using Bytes = std::vector<uint8_t>;
using cc::Why;

namespace {

// ---------------------------------------------------------------------------
// The card: a Source over bytes that counts its reads, and a clock they run
// (a read costs kReadUs and kByteUs a byte: the slices' time).
// ---------------------------------------------------------------------------
constexpr uint64_t kReadUs = 300;
constexpr double kByteUs = 0.5;  // 2 MB/s
uint64_t g_clockUs = 0;
uint64_t clockUs() { return g_clockUs; }

class Counted : public cc::Source {
public:
  explicit Counted(const Bytes& b) : b_(b) {}
  uint32_t size() const override { return static_cast<uint32_t>(b_.size()); }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    ++reads;
    bytes += n;
    g_clockUs += kReadUs + static_cast<uint64_t>(n * kByteUs);
    if (offset > b_.size() || n > b_.size() - offset) return false;
    if (n) std::memcpy(out, b_.data() + offset, n);
    return true;
  }
  uint64_t reads = 0, bytes = 0;

private:
  const Bytes& b_;
};

// ---------------------------------------------------------------------------
// The files.
// ---------------------------------------------------------------------------
// D's sections, as tagstore's reader knows them (to break one).
const cc::SectionSpec kDevSections[7] = {
    {mptg::kFold, mptg::kFoldStride, true}, {mptg::kRecs, mptg::kRecsStride, true}, {cc::kStrs, 0, true},
    {mptg::kHidx, mptg::kHidxStride, false}, {ts::kDsta, 0, true},           {ts::kDfld, 0, true},
    {ts::kDhdr, 0, true},
};
const cc::FormatSpec kDevSpec = {cc::kMagicMptg, mptg::kHeaderBytes, kDevSections, 7};
enum { kFold = 0, kRecs, kStrs, kHidx, kDsta, kDfld, kDhdr };

cc::Section sectionOf(const Bytes& b, uint32_t i, bool device) {
  cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
  cc::Container c;
  uint8_t th[mptg::kHeaderBytes];
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(c.open(src, device ? kDevSpec : mptg::kSpec, th)));
  return c.section(i);
}

// D's row for record i: a mix of the four statuses (some confirmed), and
// `odd` bytes with bits above a row's three (BuilderRows reads them as
// Pending).
uint8_t rowByte(uint32_t i, bool odd) {
  if (odd && i % 97 == 5) return 0xF1;
  ts::Row r;
  r.status = static_cast<ts::Status>(i % 7 < 4 ? 0 : i % 7 - 3);  // Scanned 4 of 7, then Software, Pending, Unreadable
  r.confirmed = r.status == ts::Status::Software && i % 2;
  return ts::encodeRow(r);
}

// D as the device's compaction lays it out: the card's records, DSTA's rows
// (rowByte()), a DFLD entry a folder, DHDR (walked against a commit).
Bytes deviceFile(const synthcard::Card& card, bool odd = false) {
  std::vector<mptg::RecordIn> recs;
  recs.reserve(card.files.size());
  std::set<std::string> folders{""};
  for (const synthcard::File& f : card.files) {
    recs.push_back(synthcard::recordOf(f, true, 0));
    for (size_t k = f.rel.find('/'); k != std::string::npos; k = f.rel.find('/', k + 1)) folders.insert(f.rel.substr(0, k));
  }
  const uint32_t n = static_cast<uint32_t>(recs.size());
  const uint32_t nf = static_cast<uint32_t>(folders.size());
  Bytes dsta(ts::kDstaHeader);
  uint32_t own = 0;
  for (uint32_t i = 0; i < n; ++i) {
    dsta.push_back(rowByte(i, odd));
    if (ts::decodeRow(dsta.back()).status != ts::Status::Software) ++own;
  }
  cc::put32(dsta.data(), n);
  cc::put32(dsta.data() + 4, own);
  cc::put32(dsta.data() + 8, nf);
  Bytes dfld(ts::kDfldHeader);
  cc::put32(dfld.data(), nf);
  for (uint32_t i = 0; i < nf; ++i) {
    uint8_t b[ts::kFactsFixed + 256];
    const uint32_t len = ts::encodeFacts(ts::FolderFacts(), b);
    dfld.insert(dfld.end(), b, b + len);
  }
  ts::DeviceHeader h;
  h.walked = true;
  h.walk.present = true;
  h.walk.cardId = 0x5EEDC0DE0A1B2C3Dull;
  h.walk.generation = 1;
  h.walk.commitId = 0x62e047dc90c90f42ull;
  h.epoch = 8;
  Bytes dhdr(ts::kDhdrBytes);
  ts::encodeHeader(h, dhdr.data());
  mptg::ExtraSection extra[3];
  extra[0].type = ts::kDsta;
  extra[0].data = dsta.data();
  extra[0].bytes = static_cast<uint32_t>(dsta.size());
  extra[1].type = ts::kDfld;
  extra[1].data = dfld.data();
  extra[1].bytes = static_cast<uint32_t>(dfld.size());
  extra[2].type = ts::kDhdr;
  extra[2].data = dhdr.data();
  extra[2].bytes = static_cast<uint32_t>(dhdr.size());
  mptg::Meta meta;
  meta.generation = 3;
  meta.cardId = 0x5EEDC0DE0A1B2C3Dull;
  meta.source = mptg::kSourceDevice;
  meta.parserVersion = 1;
  meta.producer = "mstream-player 0.10.0";
  synthcard::VecSink out;
  const char* error = nullptr;
  if (!mptg::write(out, meta, recs.data(), recs.size(), nullptr, 0, extra, 3, nullptr, &error)) TEST_FAIL_MESSAGE(error);
  return out.bytes;
}

Bytes transferFile(const synthcard::Card& card) {
  synthcard::Write w;
  return synthcard::tagsFile(card, w);
}

const synthcard::Card& smallCard() {
  static const synthcard::Card c = synthcard::make(synth::specFor(600));
  return c;
}

// N11's synthetic card's count (the device's gs of 2026-10-14).
const synthcard::Card& bigCard() {
  static const synthcard::Card c = synthcard::make(synth::specFor(19412));
  return c;
}

// ---------------------------------------------------------------------------
// gs's one call, as app/TagConsole made it before: find() (openDevice(),
// the walk with HIDX, File::open()), then status()'s rows (BuilderRows).
// ---------------------------------------------------------------------------
struct Answers {
  Why why = Why::Ok;
  bool valid = false;
  ts::DeviceInfo device;
  uint32_t rows[4] = {};
  uint32_t records = 0, folders = 0;  // File::open()'s info(), when valid
};

struct Readers {
  std::unique_ptr<mptg::Walker> walker{new mptg::Walker()};
  mptg::File file;
  ts::BuilderRows builderRows;
  ts::RowTally tally;
  Bytes scratch = Bytes(8192);  // app/TagConsole's Work
  Bytes buf = Bytes(4096);
};

Answers oneCall(cc::Source& src, bool device) {
  Answers a;
  Readers r;
  if (device) {
    a.why = ts::openDevice(src, &a.device);
    if (a.why != Why::Ok) return a;
  }
  a.why = r.walker->begin(src, mptg::kUseHidx, r.scratch.data(), static_cast<uint32_t>(r.scratch.size()));
  while (a.why == Why::Ok) {
    const mptg::Walker::Step s = r.walker->next();
    if (s == mptg::Walker::Step::End) break;
    if (s == mptg::Walker::Step::Bad) a.why = r.walker->why();
  }
  if (a.why == Why::Ok) a.why = r.file.open(src);
  if (a.why != Why::Ok) return a;
  a.valid = true;
  a.records = r.file.info().recordCount;
  a.folders = r.file.info().folderCount;
  if (device && r.builderRows.begin(src, r.buf.data(), static_cast<uint32_t>(r.buf.size()))) {
    for (uint32_t i = 0; i < a.device.tags.recordCount; ++i) ++a.rows[static_cast<int>(r.builderRows.row(i).status) & 3];
  }
  return a;
}

// The check a step at a time: its answers, and each step's reads.
struct Stepped {
  Answers a;
  uint32_t steps = 0, units = 0;
  uint64_t maxStepBytes = 0, maxStepUs = 0, bytes = 0;
};

Stepped stepped(Counted& src, bool device, const tagcheck::Config& c) {
  Stepped s;
  Readers r;
  tagcheck::Memory m;
  m.walker = r.walker.get();
  m.file = &r.file;
  m.rows = &r.tally;
  m.scratch = r.scratch.data();
  m.scratchBytes = static_cast<uint32_t>(r.scratch.size());
  m.buf = r.buf.data();
  m.bufBytes = static_cast<uint32_t>(r.buf.size());
  tagcheck::Check k;
  const uint64_t bytes0 = src.bytes;
  k.begin(src, device, m, c);
  for (;;) {
    const uint64_t b = src.bytes, t = g_clockUs;
    const tagcheck::Check::Step st = k.step();
    if (src.bytes - b > s.maxStepBytes) s.maxStepBytes = src.bytes - b;
    if (g_clockUs - t > s.maxStepUs) s.maxStepUs = g_clockUs - t;
    if (st == tagcheck::Check::Step::Done) break;
    TEST_ASSERT_TRUE_MESSAGE(k.steps() < 10000000, "the check never ends");
  }
  TEST_ASSERT_EQUAL(tagcheck::Check::Step::Done, k.step());  // (done stays done, reading nothing)
  s.bytes = src.bytes - bytes0;
  s.steps = k.steps();
  s.units = k.units();
  s.a.why = k.why();
  s.a.valid = k.valid();
  s.a.device = k.device();
  for (int i = 0; i < 4; ++i) s.a.rows[i] = k.rows()[i];
  if (k.valid()) {
    s.a.records = r.file.info().recordCount;
    s.a.folders = r.file.info().folderCount;
  }
  return s;
}

void assertSame(const Answers& want, const Answers& got, const char* what) {
  char m[160];
  snprintf(m, sizeof(m), "%s: why %s, the one call's %s", what, cc::whyName(got.why), cc::whyName(want.why));
  TEST_ASSERT_TRUE_MESSAGE(want.why == got.why, m);
  TEST_ASSERT_EQUAL_MESSAGE(want.valid, got.valid, what);
  TEST_ASSERT_EQUAL_MESSAGE(want.records, got.records, what);
  TEST_ASSERT_EQUAL_MESSAGE(want.folders, got.folders, what);
  for (int i = 0; i < 4; ++i) TEST_ASSERT_EQUAL_MESSAGE(want.rows[i], got.rows[i], what);
  const ts::DeviceInfo &x = want.device, &y = got.device;
  TEST_ASSERT_EQUAL_MESSAGE(x.present, y.present, what);
  TEST_ASSERT_EQUAL_MESSAGE(x.tags.recordCount, y.tags.recordCount, what);
  TEST_ASSERT_EQUAL_MESSAGE(x.tags.folderCount, y.tags.folderCount, what);
  TEST_ASSERT_EQUAL_MESSAGE(x.tags.parserVersion, y.tags.parserVersion, what);
  TEST_ASSERT_EQUAL_MESSAGE(x.tags.readRules, y.tags.readRules, what);
  TEST_ASSERT_EQUAL_MESSAGE(x.header.epoch, y.header.epoch, what);
  TEST_ASSERT_EQUAL_MESSAGE(x.header.walked, y.header.walked, what);
  TEST_ASSERT_TRUE_MESSAGE(x.header.walk == y.header.walk, what);
  TEST_ASSERT_EQUAL_MESSAGE(x.header.skew, y.header.skew, what);
}

// Both ways, one unit a step and in 10 ms slices: the same answers.
void checkBoth(const Bytes& file, bool device, const char* what) {
  cc::MemSource mem(file.data(), static_cast<uint32_t>(file.size()));
  const Answers want = oneCall(mem, device);
  Counted a(file);
  assertSame(want, stepped(a, device, tagcheck::Config()).a, what);
  tagcheck::Config c;
  c.sliceUs = 10000;
  c.nowUs = clockUs;
  Counted b(file);
  assertSame(want, stepped(b, device, c).a, what);
  // Pieces of one entry and one row: every piece boundary.
  c.hidxPiece = 1;
  c.rowsPiece = 1;
  Counted d(file);
  assertSame(want, stepped(d, device, c).a, what);
  // Pieces of 0: HIDX's end and the rows each in one unit.
  c.hidxPiece = 0;
  c.rowsPiece = 0;
  Counted e(file);
  assertSame(want, stepped(e, device, c).a, what);
}

Bytes withByte(Bytes b, uint32_t at, uint8_t x) {
  b[at] ^= x;
  return b;
}

// HIDX's entries i and j swapped (their bytes: the section's CRC too).
Bytes swappedHidx(Bytes b, bool device, uint32_t i, uint32_t j) {
  const cc::Section s = sectionOf(b, device ? uint32_t(kHidx) : uint32_t(mptg::kSecHidx), device);
  uint8_t t[mptg::kHidxStride];
  std::memcpy(t, &b[s.offset + i * s.stride], mptg::kHidxStride);
  std::memcpy(&b[s.offset + i * s.stride], &b[s.offset + j * s.stride], mptg::kHidxStride);
  std::memcpy(&b[s.offset + j * s.stride], t, mptg::kHidxStride);
  return b;
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// Walker::setEndPiece(): the end's HIDX check in pieces, the same verdicts.
// ---------------------------------------------------------------------------
void test_walker_end_piece_same_verdicts() {
  const Bytes t = transferFile(smallCard());
  const cc::Section h = sectionOf(t, mptg::kSecHidx, false);
  const std::vector<Bytes> files = {
      t,
      swappedHidx(t, false, 3, 4),                                // out of order
      swappedHidx(t, false, 0, h.count - 1),                      // out of order, far apart
      withByte(t, h.offset + (h.count - 1) * h.stride + 8, 0x40),  // the last entry's record out of range
      withByte(t, h.offset + 5 * h.stride + 9, 0x01),             // a record named twice, one never
      withByte(t, h.offset + h.bytes - 1, 0x80),                  // the last entry's record: high byte
      withByte(t, sectionOf(t, mptg::kSecRecs, false).offset + 20, 0x01),  // RECS' CRC
  };
  for (size_t f = 0; f < files.size(); ++f) {
    const Bytes& b = files[f];
    cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
    Bytes scratch(8192);
    const Why want = mptg::check(src, mptg::kUseHidx, scratch.data(), 8192);
    for (uint32_t piece : {1u, 7u, 100u, h.count, h.count + 1}) {
      std::unique_ptr<mptg::Walker> w(new mptg::Walker());
      Why why = w->begin(src, mptg::kUseHidx, scratch.data(), 8192);
      w->setEndPiece(piece);
      uint32_t checking = 0;
      while (why == Why::Ok) {
        const mptg::Walker::Step s = w->next();
        if (s == mptg::Walker::Step::End) break;
        if (s == mptg::Walker::Step::Bad) why = w->why();
        if (s == mptg::Walker::Step::Checking) ++checking;
      }
      char m[96];
      snprintf(m, sizeof(m), "file %u, pieces of %u: %s, check() %s", static_cast<unsigned>(f),
               static_cast<unsigned>(piece), cc::whyName(why), cc::whyName(want));
      TEST_ASSERT_TRUE_MESSAGE(why == want, m);
      if (f == 0) TEST_ASSERT_EQUAL_MESSAGE((h.count + piece - 1) / piece - 1, checking, m);
    }
    if (f == 0) TEST_ASSERT_TRUE(want == Why::Ok);
    if (f > 0) TEST_ASSERT_TRUE(want != Why::Ok);
  }
}

// begin() leaves it at 0 (the whole end in one call), whatever the last
// walk set: a walker reused by gt after gs reads as before.
void test_walker_begin_resets_end_piece() {
  const Bytes t = transferFile(smallCard());
  cc::MemSource src(t.data(), static_cast<uint32_t>(t.size()));
  Bytes scratch(8192);
  std::unique_ptr<mptg::Walker> w(new mptg::Walker());
  TEST_ASSERT_TRUE(w->begin(src, mptg::kUseHidx, scratch.data(), 8192) == Why::Ok);
  w->setEndPiece(1);
  TEST_ASSERT_TRUE(w->begin(src, mptg::kUseHidx, scratch.data(), 8192) == Why::Ok);
  for (;;) {
    const mptg::Walker::Step s = w->next();
    TEST_ASSERT_TRUE(s != mptg::Walker::Step::Checking && s != mptg::Walker::Step::Bad);
    if (s == mptg::Walker::Step::End) break;
  }
}

// ---------------------------------------------------------------------------
// RowTally: BuilderRows' rows, tallied a piece at a time.
// ---------------------------------------------------------------------------
void test_row_tally_is_builder_rows() {
  const Bytes d = deviceFile(smallCard(), true);
  const cc::Section dsta = sectionOf(d, kDsta, true);
  const std::vector<Bytes> files = {
      d,
      withByte(d, dsta.offset + ts::kDstaHeader + 17, 0x02),  // a row: DSTA's CRC
      withByte(d, dsta.offset + 12, 0x01),                    // the header's spare bytes: DSTA's CRC
      withByte(d, sectionOf(d, kDhdr, true).offset, 0x01),     // DHDR: D's frame refused
  };
  for (size_t f = 0; f < files.size(); ++f) {
    const Bytes& b = files[f];
    cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
    Bytes buf(4096);
    uint32_t want[4] = {};
    ts::DeviceInfo info;
    const bool open = ts::openDevice(src, &info) == Why::Ok;
    ts::BuilderRows rows;
    const bool begun = rows.begin(src, buf.data(), 4096);
    if (begun) {
      for (uint32_t i = 0; i < info.tags.recordCount; ++i) ++want[static_cast<int>(rows.row(i).status) & 3];
    }
    if (f == 0) {
      TEST_ASSERT_TRUE(begun);
      TEST_ASSERT_TRUE(want[0] > 0 && want[1] > 0 && want[2] > 0 && want[3] > 0);
    } else {
      TEST_ASSERT_FALSE(begun);
    }
    for (uint32_t piece : {1u, 7u, 4096u, 100000u}) {
      ts::RowTally t;
      Bytes tb(4096);
      TEST_ASSERT_EQUAL(open, t.begin(src, tb.data(), 4096));
      uint32_t steps = 0;
      while (!t.step(piece)) ++steps;
      TEST_ASSERT_TRUE(t.done());
      for (int i = 0; i < 4; ++i) TEST_ASSERT_EQUAL(want[i], t.counts()[i]);
      if (open) TEST_ASSERT_EQUAL((info.tags.recordCount + piece - 1) / piece - 1, steps);
    }
    // A piece of 0: all the rows in one step (never a step that does
    // nothing, which would hold gs, and the worker, without end).
    ts::RowTally all;
    Bytes ab(4096);
    TEST_ASSERT_EQUAL(open, all.begin(src, ab.data(), 4096));
    TEST_ASSERT_TRUE(all.step(0));
    TEST_ASSERT_TRUE(all.done());
    for (int i = 0; i < 4; ++i) TEST_ASSERT_EQUAL(want[i], all.counts()[i]);
  }
}

// ---------------------------------------------------------------------------
// The check: the one call's answers, D and T, valid and broken.
// ---------------------------------------------------------------------------
void test_check_device_same_answers() {
  const Bytes d = deviceFile(smallCard(), true);
  const cc::Section recs = sectionOf(d, kRecs, true), strs = sectionOf(d, kStrs, true);
  const cc::Section hidx = sectionOf(d, kHidx, true), dsta = sectionOf(d, kDsta, true);
  checkBoth(d, true, "D");
  checkBoth(withByte(d, recs.offset + 72 * 10 + 12, 0x01), true, "D: a record's size (RECS' CRC)");
  checkBoth(withByte(d, recs.offset + 72 * 10, 0x01), true, "D: a record's folder");
  checkBoth(withByte(d, strs.offset + strs.bytes - 3, 0x01), true, "D: a run's text (STRS' CRC)");
  checkBoth(swappedHidx(d, true, 40, 41), true, "D: HIDX out of order");
  checkBoth(withByte(d, hidx.offset + 9, 0x01), true, "D: HIDX names a record twice");
  checkBoth(withByte(d, dsta.offset + ts::kDstaHeader + 3, 0x04), true, "D: DSTA's CRC (rows all 0)");
  checkBoth(withByte(d, sectionOf(d, kDhdr, true).offset + 2, 0x01), true, "D: DHDR's CRC");
  checkBoth(withByte(d, 70, 0x01), true, "D: the header's CRC");
  checkBoth(Bytes(d.begin(), d.end() - 8), true, "D: cut short");
  checkBoth(Bytes(), true, "D: empty");
  checkBoth(transferFile(smallCard()), true, "T read as D");
  // The answers are the file's: a valid D's rows by status.
  cc::MemSource mem(d.data(), static_cast<uint32_t>(d.size()));
  const Answers a = oneCall(mem, true);
  TEST_ASSERT_TRUE(a.valid);
  TEST_ASSERT_EQUAL(smallCard().files.size(), a.rows[0] + a.rows[1] + a.rows[2] + a.rows[3]);
  TEST_ASSERT_EQUAL(8, a.device.header.epoch);
}

void test_check_transfer_same_answers() {
  const Bytes t = transferFile(smallCard());
  checkBoth(t, false, "T");
  checkBoth(swappedHidx(t, false, 0, 1), false, "T: HIDX out of order");
  checkBoth(withByte(t, sectionOf(t, mptg::kSecFold, false).offset + 4, 0x01), false, "T: a folder's name");
  checkBoth(Bytes(t.begin(), t.begin() + t.size() / 2), false, "T: half");
  checkBoth(deviceFile(smallCard()), false, "D read as T");
  // T's answers: no header of D's, no rows.
  Counted c(t);
  const Stepped s = stepped(c, false, tagcheck::Config());
  TEST_ASSERT_TRUE(s.a.valid);
  TEST_ASSERT_FALSE(s.a.device.present);
  TEST_ASSERT_EQUAL(0, s.a.rows[0] + s.a.rows[1] + s.a.rows[2] + s.a.rows[3]);
  TEST_ASSERT_EQUAL(smallCard().files.size(), s.a.records);
}

// No memory for the readers: Io at once, nothing read.
void test_check_without_memory() {
  const Bytes d = deviceFile(smallCard());
  Counted c(d);
  tagcheck::Check k;
  tagcheck::Memory m;
  k.begin(c, true, m);
  TEST_ASSERT_EQUAL(tagcheck::Check::Step::Done, k.step());
  TEST_ASSERT_TRUE(k.why() == Why::Io);
  TEST_ASSERT_FALSE(k.valid());
  TEST_ASSERT_EQUAL(0, c.reads);
}

// ---------------------------------------------------------------------------
// The bound: at 19,412 records, every step reads a few KB (a unit's), or
// keeps to its slice, where the one call read the whole file.
// ---------------------------------------------------------------------------
// The most a unit reads of a valid file: a few of the walker's refills
// (8 KB of scratch in five streams: the walk's begin reads three, and the
// header), a HIDX piece (3 KB) and a refill past it, a rows piece (2 KB)
// in one 4 KB refill, or the rows' begin (D's header, then that refill).
constexpr uint64_t kUnitBytes = 8 * 1024;

void boundedAt(const Bytes& file, bool device, const char* what) {
  cc::MemSource mem(file.data(), static_cast<uint32_t>(file.size()));
  const Answers want = oneCall(mem, device);
  TEST_ASSERT_TRUE_MESSAGE(want.valid, what);
  // The one call: the whole file in one go, D's rows twice.
  Counted once(file);
  g_clockUs = 0;
  oneCall(once, device);
  const uint64_t onceUs = g_clockUs;
  char m[200];
  snprintf(m, sizeof(m), "%s: the one call read %llu B in %llu reads (%.1f s on the clock)", what,
           static_cast<unsigned long long>(once.bytes), static_cast<unsigned long long>(once.reads), onceUs / 1e6);
  TEST_MESSAGE(m);
  TEST_ASSERT_TRUE_MESSAGE(once.bytes > 100 * kUnitBytes, m);  // so that call breaks the bound below
  // A unit a step.
  Counted a(file);
  const Stepped u = stepped(a, device, tagcheck::Config());
  assertSame(want, u.a, what);
  snprintf(m, sizeof(m), "%s: %u units, %llu B, at most %llu B a unit", what, static_cast<unsigned>(u.units),
           static_cast<unsigned long long>(u.bytes), static_cast<unsigned long long>(u.maxStepBytes));
  TEST_MESSAGE(m);
  TEST_ASSERT_TRUE_MESSAGE(u.maxStepBytes <= kUnitBytes, m);
  TEST_ASSERT_TRUE_MESSAGE(u.steps > want.records, m);  // (a step a record at least)
  // The reads spread, not added to: about the one call's (its DSTA read once
  // less), never more than a refill a unit over it.
  TEST_ASSERT_TRUE_MESSAGE(u.bytes <= once.bytes + 8192, m);
  // In 10 ms slices on the clock: each step within its slice and a unit.
  tagcheck::Config c;
  c.sliceUs = 10000;
  c.nowUs = clockUs;
  Counted b(file);
  const Stepped s = stepped(b, device, c);
  assertSame(want, s.a, what);
  snprintf(m, sizeof(m), "%s: %u steps of 10 ms (%u units), the longest %.1f ms (a unit at most %.1f ms)", what,
           static_cast<unsigned>(s.steps), static_cast<unsigned>(s.units), s.maxStepUs / 1000.0, u.maxStepUs / 1000.0);
  TEST_MESSAGE(m);
  TEST_ASSERT_TRUE_MESSAGE(s.maxStepUs <= c.sliceUs + u.maxStepUs, m);
  TEST_ASSERT_TRUE_MESSAGE(s.steps * uint64_t(c.sliceUs) >= onceUs / 2, m);  // spread over many passes
  TEST_ASSERT_TRUE_MESSAGE(s.units == u.units, m);
}

void test_check_bounded_device_at_19412() { boundedAt(deviceFile(bigCard()), true, "D at 19,412"); }

void test_check_bounded_transfer_at_19412() { boundedAt(transferFile(bigCard()), false, "T at 19,412"); }

// The end's HIDX check in one call (setEndPiece(0), the walk as before)
// reads 12 bytes a record at once: the unit the bound above needs split.
void test_whole_end_breaks_the_bound() {
  const Bytes d = deviceFile(bigCard());
  Counted src(d);
  Bytes scratch(8192);
  std::unique_ptr<mptg::Walker> w(new mptg::Walker());
  TEST_ASSERT_TRUE(w->begin(src, mptg::kUseHidx, scratch.data(), 8192) == Why::Ok);
  uint64_t most = 0;
  for (;;) {
    const uint64_t b = src.bytes;
    const mptg::Walker::Step s = w->next();
    if (src.bytes - b > most) most = src.bytes - b;
    TEST_ASSERT_TRUE(s != mptg::Walker::Step::Bad && s != mptg::Walker::Step::Checking);
    if (s == mptg::Walker::Step::End) break;
  }
  TEST_ASSERT_TRUE(most >= 12ull * bigCard().files.size() - 8192);
  TEST_ASSERT_TRUE(most > kUnitBytes);
}

// ---------------------------------------------------------------------------
// gs's wait for the worker's step under way (app/TagConsole's Wait).
// ---------------------------------------------------------------------------
void test_wait_over() {
  using tagcheck::waitOver;
  const uint32_t start = 1000, max = 10000;
  // The worker free: at once, whatever the time.
  TEST_ASSERT_TRUE(waitOver(false, start, start, max));
  TEST_ASSERT_TRUE(waitOver(false, start, start - 1, max));
  // A step under way: until it's taken in, or max.
  TEST_ASSERT_FALSE(waitOver(true, start, start, max));
  TEST_ASSERT_FALSE(waitOver(true, start, start + max - 1, max));
  TEST_ASSERT_TRUE(waitOver(true, start, start + max, max));
  TEST_ASSERT_TRUE(waitOver(true, start, start + 3 * max, max));
  // The pass's time, taken before gs's millis() in that pass: no time
  // passed (as unsigned, 49 days: the wait was skipped, 2026-10-14's
  // review).
  TEST_ASSERT_FALSE(waitOver(true, start, start - 1, max));
  TEST_ASSERT_FALSE(waitOver(true, start, start - 25, max));
  // Across millis()'s wrap.
  const uint32_t late = 0xFFFFFF00u;
  TEST_ASSERT_FALSE(waitOver(true, late, late + 512, max));  // (wrapped: 0x100)
  TEST_ASSERT_FALSE(waitOver(true, late, late - 1, max));
  TEST_ASSERT_TRUE(waitOver(true, late, late + max, max));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_walker_end_piece_same_verdicts);
  RUN_TEST(test_walker_begin_resets_end_piece);
  RUN_TEST(test_row_tally_is_builder_rows);
  RUN_TEST(test_check_device_same_answers);
  RUN_TEST(test_check_transfer_same_answers);
  RUN_TEST(test_check_without_memory);
  RUN_TEST(test_check_bounded_device_at_19412);
  RUN_TEST(test_check_bounded_transfer_at_19412);
  RUN_TEST(test_whole_end_breaks_the_bound);
  RUN_TEST(test_wait_over);
  return UNITY_END();
}
