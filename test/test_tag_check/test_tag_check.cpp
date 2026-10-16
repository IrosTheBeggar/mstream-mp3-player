// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the console's gs a piece a loop pass (lib/core TagCheck,
// mptg::Walker::setEndPiece(), tagstore::RowTally): the same answers as the
// one call gs made before (app/TagConsole's find() and status()'s rows:
// openDevice(), the walk with HIDX, File::open(), BuilderRows) on D and T,
// valid and broken; every step's reads bounded, at N11's 19,412 records,
// where that one call read the whole of D (it held the loop 12.4 s on the
// device, 2026-10-14); gs's wait for the worker's step under way
// (tagcheck::waitOver()); and gs over the loop's passes, the phases
// app/TagConsole runs (tagcheck::Gs over a card in memory, its GsIo):
// never reading D while a compaction replaces it, reading a file again if
// one did or the card was mounted again under it (tagcheck::Turn: the
// device's false ABSENT of 2026-10-14), T read whatever D's wait came to,
// its pieces going on in the dark with a list stuck "moving"
// (tagcheck::pieceWaits()). Run: pio test -e native
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
// (rowByte()), a DFLD entry a folder, DHDR (walked against a commit; its
// epoch `epoch`).
Bytes deviceFile(const synthcard::Card& card, bool odd = false, uint32_t epoch = 8) {
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
  h.epoch = epoch;
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

// ---------------------------------------------------------------------------
// gs over the loop's passes (tagcheck::Gs, which app/TagConsole runs: its
// first lines after waitOver(), then D's turn, tagcheck::Turn, a piece a
// pass unless pieceWaits(), its journals, T's root and turn, the plan), on
// a card where D is replaced, or the card mounted again under a read.
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t kWaitMs = 10000;  // app/TagConsole's

uint32_t nowMs() { return static_cast<uint32_t>(g_clockUs / 1000); }

// The card under gs: D is `before` until a compaction puts `after` in its
// place, T is `transfer` (none: no transfer data; no compaction replaces
// it). The compaction is under way from fromUs to toUs (the worker's step,
// CardTasks::replacing(); its end puts `after` in place). FatFs mounts the
// card again at its remountAtRead-th read (the card's guard counts it): a
// handle opened before reads nothing after it. A D handle opened before
// the compaction's end reads zeros after it (the clusters it had were
// freed: on the device, whatever they hold next).
struct Card {
  const Bytes* before = nullptr;
  const Bytes* after = nullptr;
  const Bytes* transfer = nullptr;
  uint64_t fromUs = UINT64_MAX, toUs = UINT64_MAX;
  uint64_t remountAtRead = 0;
  uint32_t remounts = 0;
  uint64_t reads = 0, readsUnder = 0, readsLost = 0;

  bool replacing() const { return g_clockUs >= fromUs && g_clockUs < toUs; }
  const Bytes& device() const { return g_clockUs >= toUs ? *after : *before; }
  // What app/TagConsole::pass() says (the compaction the only step).
  tagcheck::Pass pass(bool listMoving, bool dark) const {
    tagcheck::Pass p;
    p.nowMs = nowMs();
    p.workerBusy = p.replacing = replacing();
    p.compactions = g_clockUs >= fromUs ? 1 : 0;
    p.remounts = remounts;
    p.listMoving = listMoving;
    p.dark = dark;
    return p;
  }
};

// A file open on the card (app/TagConsole's FileAt over an fs::File): its
// reads cost what Counted's do.
class Handle : public cc::Source {
public:
  Handle(Card& c, bool device)
      : card_(c), device_(device), file_(device ? &c.device() : c.transfer), remounts_(c.remounts) {}
  uint32_t size() const override { return static_cast<uint32_t>(file_->size()); }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    g_clockUs += kReadUs + static_cast<uint64_t>(n * kByteUs);
    ++card_.reads;
    if (device_ && card_.replacing()) ++card_.readsUnder;
    if (card_.remountAtRead && card_.reads == card_.remountAtRead) ++card_.remounts;
    if (card_.remounts != remounts_) {
      ++card_.readsLost;
      return false;  // its handle gone (FatFs: FR_INVALID_OBJECT)
    }
    if (offset > file_->size() || n > file_->size() - offset) return false;
    if (device_ && &card_.device() != file_) {
      if (n) std::memset(out, 0, n);  // replaced since the open
      return true;
    }
    if (n) std::memcpy(out, file_->data() + offset, n);
    return true;
  }

private:
  Card& card_;
  bool device_;
  const Bytes* file_;
  uint32_t remounts_;
};

// gs's reads and lines on the card (app/TagConsole's GsIo): each call a
// word in `log`, the files' answers and when their lines came.
class Io final : public tagcheck::GsIo {
public:
  explicit Io(Card& c) : card(c) {
    m_.walker = rd_.walker.get();
    m_.file = &rd_.file;
    m_.rows = &rd_.tally;
    m_.scratch = rd_.scratch.data();
    m_.scratchBytes = static_cast<uint32_t>(rd_.scratch.size());
    m_.buf = rd_.buf.data();
    m_.bufBytes = static_cast<uint32_t>(rd_.buf.size());
    c_.sliceUs = 10000;  // app/TagConsole's kSliceUs
    c_.nowUs = clockUs;
  }
  void hold(tagcheck::Hold h) override {
    say(h == tagcheck::Hold::Worker ? "hold W" : h == tagcheck::Hold::Records ? "hold R" : "hold N");
  }
  void firstLines() override {
    say("lines");
    linesAtMs = nowMs();
    replacingAtLines = card.replacing();
  }
  bool memory() override { return haveMemory; }
  tagcheck::Check& check() override { return k_; }
  bool open(bool device) override {
    say(device ? "D open" : "T open");
    if (device && dMissing) return false;
    h_.reset(new Handle(card, device));
    k_.begin(*h_, device, m_, c_);
    return true;
  }
  void close() override { h_.reset(); }
  void lines(bool device, bool read) override {
    say(device ? (read ? "D lines" : "D none") : (read ? "T lines" : "T none"));
    Answers& a = device ? d : t;
    a = Answers();
    if (read) {
      a.why = k_.why();
      a.valid = k_.valid();
      a.device = k_.device();
      for (int i = 0; i < 4; ++i) a.rows[i] = k_.rows()[i];
      if (k_.valid()) {
        a.records = rd_.file.info().recordCount;
        a.folders = rd_.file.info().folderCount;
      }
    }
    (device ? dAtMs : tAtMs) = nowMs();
  }
  void waiting(bool device) override { say(device ? "D wait" : "T wait"); }
  void again(bool device, const tagcheck::Turn& turn) override {
    say(device ? "D again" : "T again");
    remounted = turn.remounted();
  }
  void notRead(bool device, const tagcheck::Turn& turn) override {
    say(device ? "D not read" : "T not read");
    waitedOut = turn.waitedOut();
  }
  void journals() override { say("journals"); }
  bool root() override {
    say("root");
    return card.transfer != nullptr;
  }
  void plan() override { say("plan"); }
  void end() override { say("end"); }

  uint32_t count(const char* word) const {
    uint32_t n = 0;
    for (const std::string& w : words)
      if (w == word) ++n;
    return n;
  }

  Card& card;
  bool haveMemory = true, dMissing = false;
  std::vector<std::string> words;
  std::string log;  // the words, ", " between
  uint32_t linesAtMs = UINT32_MAX, dAtMs = UINT32_MAX, tAtMs = UINT32_MAX;
  bool replacingAtLines = false, remounted = false, waitedOut = false;
  Answers d, t;

private:
  void say(const char* w) {
    words.push_back(w);
    if (!log.empty()) log += ", ";
    log += w;
  }

  Readers rd_;
  tagcheck::Memory m_;
  tagcheck::Config c_;
  tagcheck::Check k_;
  std::unique_ptr<Handle> h_;
};

struct GsRun {
  bool over = false;
  uint32_t startMs = 0, overAfterMs = 0, pieces = 0;
};

// gs as app/TagConsole runs it: begun and its first piece in the command's
// pass (status()), then a pass every `sleepUs` (the loop's sleep: about 5
// ms lit, 20 ms dark), a piece unless pieceWaits() (loop()), the list's
// state and the screen's as given; until it's over, or `forUs` passed.
GsRun runGs(Io& io, uint64_t sleepUs, bool listMoving, bool dark, uint64_t forUs) {
  GsRun r;
  tagcheck::Gs gs;
  const uint64_t endUs = g_clockUs + forUs;
  r.startMs = nowMs();
  gs.begin(r.startMs, kWaitMs, io);
  gs.step(io.card.pass(listMoving, dark), io);
  while (!gs.idle() && g_clockUs < endUs) {
    g_clockUs += sleepUs;
    const tagcheck::Pass p = io.card.pass(listMoving, dark);
    if (tagcheck::pieceWaits(p)) continue;
    ++r.pieces;
    gs.step(p, io);
  }
  r.over = gs.idle();
  r.overAfterMs = nowMs() - r.startMs;
  return r;
}

Answers answersOf(const Bytes& file, bool device = true) {
  cc::MemSource mem(file.data(), static_cast<uint32_t>(file.size()));
  return oneCall(mem, device);
}

const char* const kWhole = "hold W, lines, hold R, D open, D lines, journals, root, T open, T lines, plan, end, hold N";

}  // namespace

// FINDING A of the 2026-10-14 device run (check 3): a compaction under way
// (the scan's, 33.1 s) when gs came, 5 s into it. gs's first lines came at
// its 10 s cap with the compaction still under way, and its check then read
// tags.bin as the compaction put the new one in its place: "D ...: ABSENT,
// it fails its checks (string)" 24 ms after "[card] compaction done", for a
// D the next gs read whole. Now D's turn waits for the compaction's end,
// says so once, reads nothing meanwhile, and reads the new D whole; then T
// as ever. Twice: at the device's timing, and with the compaction's end
// 1.5 s into what was the read (the device's read of D took 32 s lit, the
// host clock's about 4: so the old order puts the replacement inside it,
// as on the device).
void test_gs_waits_out_a_compaction_past_its_wait() {
  const Bytes before = deviceFile(bigCard(), false, 8), after = deviceFile(bigCard(), false, 9);
  const Bytes transfer = transferFile(smallCard());
  const Answers want = answersOf(after), wantT = answersOf(transfer, false);
  TEST_ASSERT_TRUE(want.valid);
  TEST_ASSERT_TRUE(wantT.valid);
  const struct {
    const char* name;
    uint64_t gsInUs;  // gs's command, into the compaction
  } runs[] = {
      {"the device's timing (gs 5.1 s into it)", 5085000ull},
      {"its end 1.5 s into the old order's read", 33100000ull - (kWaitMs + 1500) * 1000ull},
  };
  constexpr int kRuns = sizeof(runs) / sizeof(runs[0]);
  Card cards[kRuns];
  std::unique_ptr<Io> ios[kRuns];
  GsRun got[kRuns];
  uint32_t leftMs[kRuns];
  char m[kRuns][400];
  for (int i = 0; i < kRuns; ++i) {
    Card& card = cards[i];
    card.before = &before;
    card.after = &after;
    card.transfer = &transfer;
    g_clockUs = 1161800000ull;  // the compaction began (the device's 1161.8 s)
    card.fromUs = g_clockUs;
    card.toUs = g_clockUs + 33100000ull;  // ... and took 33.1 s
    g_clockUs += runs[i].gsInUs;          // gs
    leftMs[i] = static_cast<uint32_t>((card.toUs - g_clockUs) / 1000);
    ios[i].reset(new Io(card));
    const Io& io = *ios[i];
    const GsRun& r = got[i] = runGs(*ios[i], 5000, false, false, 600000000ull);
    char d[48];
    if (io.count("D lines") != 1) {
      snprintf(d, sizeof(d), "%s", io.count("D not read") ? "not read" : "no line");
    } else if (io.d.valid) {
      snprintf(d, sizeof(d), "valid, epoch %lu", (unsigned long)io.d.device.header.epoch);
    } else {
      snprintf(d, sizeof(d), "ABSENT, it fails its checks (%s)", cc::whyName(io.d.why));
    }
    snprintf(m[i], sizeof(m[i]),
             "%s: lines at +%lu ms, %lu said, %lu opens, %lu again, %llu reads under it, its line at +%lu ms: D %s "
             "[%s]",
             runs[i].name, (unsigned long)(io.linesAtMs - r.startMs), (unsigned long)io.count("D wait"),
             (unsigned long)io.count("D open"), (unsigned long)io.count("D again"), (unsigned long long)card.readsUnder,
             (unsigned long)(io.dAtMs - r.startMs), d, io.log.c_str());
    TEST_MESSAGE(m[i]);
  }
  for (int i = 0; i < kRuns; ++i) {
    const Io& io = *ios[i];
    const GsRun& r = got[i];
    const char* msg = m[i];
    // Its first lines at the cap, the compaction under way (the old gs read
    // from there).
    TEST_ASSERT_TRUE_MESSAGE(io.linesAtMs - r.startMs >= kWaitMs && io.linesAtMs - r.startMs <= kWaitMs + 5, msg);
    TEST_ASSERT_TRUE_MESSAGE(io.replacingAtLines, msg);
    // D: waited for, said once, nothing read under the compaction, read once
    // after it: the new file's answers, never ABSENT; then T, the rest.
    TEST_ASSERT_TRUE_MESSAGE(r.over, msg);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        "hold W, lines, hold R, D wait, D open, D lines, journals, root, T open, T lines, plan, end, hold N",
        io.log.c_str(), msg);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0, cards[i].readsUnder, msg);
    TEST_ASSERT_TRUE_MESSAGE(io.dAtMs - r.startMs > leftMs[i], msg);  // (after the compaction's end)
    assertSame(want, io.d, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(9, io.d.device.header.epoch, msg);
    assertSame(wantT, io.t, msg);
  }
}

// FINDING 1 of the fix's review: the card mounted again (by FatFs: a card
// that stopped answering, or pulled and put back) as a file was read. The
// handle gs opened before it is gone, its reads fail (`io`), and the check
// would call a whole file absent; the remount (the card's guard's count)
// says it's read again, D or T. No compaction is under way.
void test_gs_reads_again_a_file_the_card_mounted_again_under() {
  const Bytes dFile = deviceFile(smallCard(), false, 8), transfer = transferFile(smallCard());
  const Answers want = answersOf(dFile), wantT = answersOf(transfer, false);
  // Undisturbed: the reads D's check takes (a gs without T), then T's.
  Card dOnly, both;
  dOnly.before = dOnly.after = both.before = both.after = &dFile;
  both.transfer = &transfer;
  g_clockUs = 0;
  Io dIo(dOnly);
  TEST_ASSERT_TRUE(runGs(dIo, 20000, false, true, 120000000ull).over);
  g_clockUs = 0;
  Io bothIo(both);
  TEST_ASSERT_TRUE(runGs(bothIo, 20000, false, true, 120000000ull).over);
  TEST_ASSERT_EQUAL_STRING(kWhole, bothIo.log.c_str());
  TEST_ASSERT_TRUE(dOnly.reads > 60);               // (the remount below falls inside D's check)
  TEST_ASSERT_TRUE(both.reads - dOnly.reads > 60);  // (... and T's)
  // The remount 30 reads into D's check, then 30 into T's.
  for (int file = 0; file < 2; ++file) {
    Card card;
    card.before = card.after = &dFile;
    card.transfer = &transfer;
    card.remountAtRead = 30 + (file == 1 ? dOnly.reads : 0);
    g_clockUs = 0;
    Io io(card);
    const GsRun r = runGs(io, 20000, false, true, 120000000ull);
    const char* m = io.log.c_str();
    TEST_MESSAGE(m);
    TEST_ASSERT_TRUE_MESSAGE(r.over, m);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, card.remounts, m);
    TEST_ASSERT_TRUE_MESSAGE(card.readsLost > 0, m);  // (its reads failed: the check's answer was `io`)
    TEST_ASSERT_TRUE_MESSAGE(io.remounted, m);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        file == 0 ? "hold W, lines, hold R, D open, D again, D open, D lines, journals, root, T open, T lines, plan, "
                    "end, hold N"
                  : "hold W, lines, hold R, D open, D lines, journals, root, T open, T again, T open, T lines, plan, "
                    "end, hold N",
        m, m);
    assertSame(want, io.d, "D after the remount");
    assertSame(wantT, io.t, "T after the remount");
  }
}

// A compaction that never ends (a worker stuck): gs waits kReplaceWaitMs
// after it said so, then leaves D unread, never calling it absent. T, which
// no compaction replaces, is read then all the same (FINDING 2 of the
// fix's review: T's turn took D's give-up, and printed NOT READ unopened).
void test_gs_gives_up_on_a_replacement_that_never_ends() {
  const Bytes d = deviceFile(smallCard()), transfer = transferFile(smallCard());
  const Answers wantT = answersOf(transfer, false);
  Card card;
  card.before = card.after = &d;
  card.transfer = &transfer;
  g_clockUs = 5000000ull;
  card.fromUs = g_clockUs;  // (toUs: never)
  Io io(card);
  const GsRun r = runGs(io, 20000, false, true, 3600000000ull);
  const char* m = io.log.c_str();
  TEST_ASSERT_TRUE_MESSAGE(r.over, m);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(
      "hold W, lines, hold R, D wait, D not read, journals, root, T open, T lines, plan, end, hold N", m, m);
  TEST_ASSERT_TRUE(io.waitedOut);
  TEST_ASSERT_EQUAL_UINT64(0, card.readsUnder);  // (D: none; T's reads aren't D's)
  TEST_ASSERT_TRUE(io.tAtMs - r.startMs >= kWaitMs + tagcheck::kReplaceWaitMs);
  TEST_ASSERT_TRUE(io.tAtMs - r.startMs <= kWaitMs + tagcheck::kReplaceWaitMs + 1000);
  assertSame(wantT, io.t, "T after D's give-up");
}

// gs's phases otherwise: no memory for the reads (gs ends after its first
// lines), D not there (its line says so), no transfer data (T's turn
// skipped); the worker held from the command to the last line.
void test_gs_phases() {
  const Bytes d = deviceFile(smallCard());
  const Answers want = answersOf(d);
  {
    Card card;
    card.before = card.after = &d;
    g_clockUs = 0;
    Io io(card);
    io.haveMemory = false;
    const GsRun r = runGs(io, 5000, false, false, 60000000ull);
    TEST_ASSERT_TRUE(r.over);
    TEST_ASSERT_EQUAL_STRING("hold W, lines, hold R, end, hold N", io.log.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, r.pieces);  // (over in the command's pass)
    TEST_ASSERT_EQUAL_UINT64(0, card.reads);
  }
  {
    Card card;
    card.before = card.after = &d;
    g_clockUs = 0;
    Io io(card);
    io.dMissing = true;
    TEST_ASSERT_TRUE(runGs(io, 5000, false, false, 60000000ull).over);
    TEST_ASSERT_EQUAL_STRING("hold W, lines, hold R, D open, D none, journals, root, plan, end, hold N",
                             io.log.c_str());
  }
  {
    Card card;
    card.before = card.after = &d;
    g_clockUs = 0;
    Io io(card);
    TEST_ASSERT_TRUE(runGs(io, 5000, false, false, 60000000ull).over);
    TEST_ASSERT_EQUAL_STRING("hold W, lines, hold R, D open, D lines, journals, root, plan, end, hold N",
                             io.log.c_str());
    assertSame(want, io.d, "D");
  }
  // A step under way that isn't a compaction (the worker busy alone): the
  // first lines once it's taken in, and D read at once.
  {
    Card card;
    card.before = card.after = &d;
    g_clockUs = 0;
    Io io(card);
    tagcheck::Gs gs;
    gs.begin(0, kWaitMs, io);
    tagcheck::Pass p = card.pass(false, false);
    p.workerBusy = true;
    gs.step(p, io);
    p.nowMs = 3000;
    gs.step(p, io);
    TEST_ASSERT_EQUAL_STRING("hold W", io.log.c_str());
    TEST_ASSERT_EQUAL(tagcheck::Gs::Phase::Wait, gs.phase());
    p.workerBusy = false;
    gs.step(p, io);
    TEST_ASSERT_EQUAL_STRING("hold W, lines, hold R", io.log.c_str());
    gs.step(p, io);
    TEST_ASSERT_EQUAL_STRING("hold W, lines, hold R, D open", io.log.c_str());
    TEST_ASSERT_EQUAL(tagcheck::Gs::Phase::Device, gs.phase());
  }
}

// The turn's rules: a check's answer stands only if nothing replaced the
// file since its open and the card wasn't mounted again; kRereads reads
// again at most, then it gives up; T's turn waits for no compaction and
// counts none, and a new turn starts afresh; a wait's time from a pass
// earlier than its start is none.
void test_turn_rules() {
  using Now = tagcheck::Turn::Now;
  const Bytes empty;
  Counted src(empty);
  tagcheck::Check k;  // (no memory: each check ends at its first step)
  tagcheck::Turn t;
  tagcheck::Pass p;
  const auto read = [&](bool device) {
    TEST_ASSERT_EQUAL(Now::Open, t.pass(p, k));
    k.begin(src, device, tagcheck::Memory());
    t.opened(p);
    TEST_ASSERT_EQUAL(Now::Working, t.pass(p, k));  // (its last step: judged at the next pass)
  };
  t.begin(true);
  read(true);
  TEST_ASSERT_EQUAL(Now::Done, t.pass(p, k));
  // A compaction handed under each read: read again twice, then given up.
  for (uint32_t n = 1; n <= tagcheck::kRereads + 1; ++n) {
    read(true);
    ++p.compactions;  // (handed in that pass, after its reads: seen at the next)
    TEST_ASSERT_EQUAL(n <= tagcheck::kRereads ? Now::Again : Now::GaveUp, t.pass(p, k));
  }
  TEST_ASSERT_FALSE(t.waitedOut());
  TEST_ASSERT_FALSE(t.remounted());
  TEST_ASSERT_EQUAL_UINT32(tagcheck::kRereads + 1, t.rereads());
  TEST_ASSERT_EQUAL(Now::GaveUp, t.pass(p, k));  // (it stays given up)
  // The card mounted again under a read: read again, D or T.
  for (int device = 1; device >= 0; --device) {
    t.begin(device == 1);
    read(device == 1);
    ++p.remounts;
    TEST_ASSERT_EQUAL(Now::Again, t.pass(p, k));
    TEST_ASSERT_TRUE(t.remounted());
    read(device == 1);
    TEST_ASSERT_EQUAL(Now::Done, t.pass(p, k));
  }
  // T's turn: a compaction under way, or handed under its read, is nothing
  // to it.
  t.begin(false);
  p.replacing = true;
  read(false);
  ++p.compactions;
  TEST_ASSERT_EQUAL(Now::Done, t.pass(p, k));
  // D's: one under way when the check ends: read again ...
  t.begin(true);
  p.replacing = false;
  read(true);
  p.replacing = true;
  TEST_ASSERT_EQUAL(Now::Again, t.pass(p, k));
  TEST_ASSERT_FALSE(t.remounted());
  // ... and the next pass waits: said once, its time from then.
  p.nowMs = 1000;
  TEST_ASSERT_EQUAL(Now::Said, t.pass(p, k));
  p.nowMs = 999;  // (a pass's time before the wait's own: none passed)
  TEST_ASSERT_EQUAL(Now::Wait, t.pass(p, k));
  p.nowMs = 1000 + tagcheck::kReplaceWaitMs - 1;
  TEST_ASSERT_EQUAL(Now::Wait, t.pass(p, k));
  p.replacing = false;  // over: read at once, and a new wait says so again
  TEST_ASSERT_EQUAL(Now::Open, t.pass(p, k));
  p.replacing = true;
  TEST_ASSERT_EQUAL(Now::Said, t.pass(p, k));
  p.nowMs += tagcheck::kReplaceWaitMs;
  TEST_ASSERT_EQUAL(Now::GaveUp, t.pass(p, k));
  TEST_ASSERT_TRUE(t.waitedOut());
  // T's turn after it, the compaction still under way: read, afresh.
  t.begin(false);
  TEST_ASSERT_FALSE(t.waitedOut());
  TEST_ASSERT_EQUAL(Now::Open, t.pass(p, k));
  TEST_ASSERT_EQUAL(0, src.reads);
}

// FINDING B's gate for gs's pieces: none while a list moves on a lit
// screen; in the dark a list "moving" (its page owing the wake a frame:
// a reload in the dark) holds none (2026-10-14: the library update's save
// waited 260 s on that gate, with the screen off; gs's pieces use it).
void test_gs_pieces_in_the_dark() {
  const Bytes d = deviceFile(smallCard());
  const Answers want = answersOf(d);
  {
    // Lit, the list moving all along: its first lines (the command's own
    // pass), then no piece, nothing read.
    Card card;
    card.before = card.after = &d;
    g_clockUs = 0;
    Io io(card);
    const GsRun r = runGs(io, 5000, true, false, 120000000ull);
    TEST_ASSERT_FALSE(r.over);
    TEST_ASSERT_EQUAL_UINT32(0, r.pieces);
    TEST_ASSERT_EQUAL_UINT64(0, card.reads);
    TEST_ASSERT_EQUAL_STRING("hold W, lines, hold R", io.log.c_str());
  }
  {
    // Dark, the list "moving" all along: gs reads on to its end.
    Card card;
    card.before = card.after = &d;
    g_clockUs = 0;
    Io io(card);
    const GsRun r = runGs(io, 20000, true, true, 120000000ull);
    TEST_ASSERT_TRUE_MESSAGE(r.over, "gs in the dark, a list owing its frame");
    TEST_ASSERT_EQUAL_STRING("hold W, lines, hold R, D open, D lines, journals, root, plan, end, hold N",
                             io.log.c_str());
    assertSame(want, io.d, "D in the dark");
  }
  // The rule.
  tagcheck::Pass p;
  TEST_ASSERT_FALSE(tagcheck::pieceWaits(p));
  p.listMoving = true;
  TEST_ASSERT_TRUE(tagcheck::pieceWaits(p));
  p.dark = true;
  TEST_ASSERT_FALSE(tagcheck::pieceWaits(p));
  p.listMoving = false;
  TEST_ASSERT_FALSE(tagcheck::pieceWaits(p));
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
  RUN_TEST(test_gs_waits_out_a_compaction_past_its_wait);
  RUN_TEST(test_gs_reads_again_a_file_the_card_mounted_again_under);
  RUN_TEST(test_gs_gives_up_on_a_replacement_that_never_ends);
  RUN_TEST(test_gs_phases);
  RUN_TEST(test_turn_rules);
  RUN_TEST(test_gs_pieces_in_the_dark);
  return UNITY_END();
}
