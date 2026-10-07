// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for LibraryBuilder (docs/METADATA.md 2.9, 3.4): the rules per
// file (2.18's builder vectors), the merge of T and D read from the shared
// fixtures and from the tagged synthetic library, the restarts, the inputs
// of library.idx v6, Stage A's votes (5.4), the library roots, the transfer
// thumbnails, and the 20k memory, build peak and T-against-D equality.
// Run: pio test -e native
#include <unity.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../support/CardFixtures.h"
#include "../support/SynthCard.h"
#include "CardContract.h"
#include "CardTags.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"
#include "LibrarySynth.h"

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;
using B = LibraryBuilder;

namespace {

// A counting allocator for every block the index and the builder take; it
// shrinks in place, as the firmware's heap_caps_realloc does.
struct Heap {
  static size_t live, peak, limit;
  static std::vector<std::pair<void*, size_t>>& blocks() {
    static std::vector<std::pair<void*, size_t>> b;
    return b;
  }
  static void reset() {
    live = peak = 0;
    limit = SIZE_MAX;
    blocks().clear();
  }
  static void* alloc(size_t n) {
    if (live + n > limit) return nullptr;
    void* p = std::malloc(n ? n : 1);
    blocks().push_back({p, n});
    live += n;
    if (live > peak) peak = live;
    return p;
  }
  static void release(void* p) {
    for (size_t i = blocks().size(); i-- > 0;) {
      if (blocks()[i].first == p) {
        live -= blocks()[i].second;
        blocks().erase(blocks().begin() + static_cast<long>(i));
        std::free(p);
        return;
      }
    }
    TEST_FAIL_MESSAGE("freed a block nobody allocated");
  }
  static void* shrink(void* p, size_t n) {
    for (auto& b : blocks()) {
      if (b.first == p) {
        live -= b.second - n;
        b.second = n;
        return p;
      }
    }
    return nullptr;
  }
};
size_t Heap::live = 0;
size_t Heap::peak = 0;
size_t Heap::limit = SIZE_MAX;

std::string bytesOf(const char* rel) { return cardfixtures::bytes(rel); }

uint32_t findPath(const LibraryIndex& idx, const char* rel) {
  return idx.findTrack((std::string("/music/") + rel).c_str());
}

std::string title(const LibraryIndex& idx, uint32_t t) {
  uint8_t len;
  const char* s = idx.trackTitle(t, &len);
  return std::string(s, len);
}

uint32_t source(const LibraryIndex& idx, uint32_t t) { return idx.track(t).flags & LibraryIndex::kSourceMask; }

uint32_t findAlbumByFolder(const LibraryIndex& idx, const char* path) {
  char buf[300];
  for (uint32_t a = 0; a < idx.albumCount(); ++a) {
    idx.folderPath(idx.album(a).folder, buf, sizeof(buf));
    if (std::strcmp(buf, path) == 0) return a;
  }
  return LibraryIndex::kNone;
}

// A tags file of a few made-up records, for the merge's edge cases.
struct Rec {
  const char* path;
  uint32_t size;
  uint32_t fatTime;
  const char* title = nullptr;
  uint16_t flags = 0;
  uint32_t known = mptg::kKnownRules1;
};
std::vector<uint8_t> writeTags(uint8_t source, const std::vector<Rec>& rs, uint16_t parserVersion = 1) {
  std::vector<mptg::RecordIn> in(rs.size());
  for (size_t i = 0; i < rs.size(); ++i) {
    in[i].path = rs[i].path;
    in[i].rec.size = rs[i].size;
    in[i].rec.fatTime = rs[i].fatTime;
    in[i].rec.flags = rs[i].flags;
    in[i].rec.known = rs[i].title ? rs[i].known : 0;
    in[i].rec.container = 1;
    in[i].fields[cc::kTitle] = rs[i].title;
  }
  mptg::Meta meta;
  meta.source = source;
  meta.generation = 7;
  meta.parserVersion = parserVersion;
  synthcard::VecSink out;
  const char* error = nullptr;
  TEST_ASSERT_TRUE_MESSAGE(mptg::write(out, meta, in.data(), in.size(), nullptr, 0, nullptr, 0, nullptr, &error),
                           error ? error : "write");
  return out.bytes;
}

constexpr uint32_t kT = 0x5D4773D5;  // 2026-10-07 14:30:42

uint32_t shifted(uint32_t t, int32_t s) { return cc::fatTimeFromWall(cc::fatWallSeconds(t) + s); }

}  // namespace

void setUp() { Heap::reset(); }
void tearDown() {}

// ---- 2.9, file by file (2.18's builderPrecedence vectors) ----

void test_precedence_vectors() {
  // The vectors are in the shared fixtures as text: seven of them.
  minijson::Value v;
  TEST_ASSERT_TRUE(cardfixtures::json("vectors.json", &v));
  TEST_ASSERT_EQUAL_size_t(7, v["builderPrecedence"]["cases"].size());

  const uint32_t s = 4000000;
  auto seen = [](uint32_t size, uint32_t t, uint16_t flags = 0) {
    B::Seen x;
    x.present = true;
    x.size = size;
    x.fatTime = t;
    x.flags = flags;
    return x;
  };
  const B::Seen none;
  B::Row software;
  software.status = B::Status::Software;
  B::Row scanned;
  B::Row pending;
  pending.status = B::Status::Pending;
  B::Row unreadable;
  unreadable.status = B::Status::Unreadable;
  B::Row confirmed = software;
  confirmed.confirmed = true;
  auto is = [](B::Choice c, B::Pick p, bool pend) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(p), static_cast<int>(c.pick));
    TEST_ASSERT_EQUAL(pend, c.pending);
  };

  // 1. The walk (s, t) and T (s, t): T. (D's row is the walk's sight.)
  is(B::choose(seen(s, kT), seen(s, kT), software, true, 0, false), B::Pick::Transfer, false);
  // 2. T (s, t - 3600) under a skew of +3600: T; without the skew, no match.
  is(B::choose(seen(s, shifted(kT, -3600)), seen(s, kT), software, true, 3600, false), B::Pick::Transfer, false);
  is(B::choose(seen(s, shifted(kT, -3600)), seen(s, kT), software, true, 0, false), B::Pick::Path, true);
  // 3. T (s, t - 2), no skew: qfp decides; confirmed (the walk saved it in
  //    D's row): T; not confirmed: D's own record if it has one, else the path.
  is(B::choose(seen(s, kT - 1), seen(s, kT), confirmed, true, 0, false), B::Pick::Transfer, false);
  is(B::choose(seen(s, kT - 1), seen(s, kT), scanned, true, 0, false), B::Pick::Device, false);
  is(B::choose(seen(s, kT - 1), seen(s, kT), pending, true, 0, false), B::Pick::Path, true);
  // A confirmation is for a size: a size that differs never matches.
  is(B::choose(seen(s + 1, kT), seen(s, kT), confirmed, true, 0, false), B::Pick::Path, true);
  // 4. The listener retagged a software file: the file is (s + 1, t), T
  //    still says (s, t), and D's scan of it says (s + 1, t): D.
  is(B::choose(seen(s, kT), seen(s + 1, kT), scanned, true, 0, false), B::Pick::Device, false);
  // 5. T UNREADABLE without FROM_API, and the device read the tags: D. With
  //    FROM_API it names the file; without a D record, the scan may read it.
  is(B::choose(seen(s, kT, mptg::kUnreadable), seen(s, kT), scanned, true, 0, false), B::Pick::Device, false);
  is(B::choose(seen(s, kT, mptg::kUnreadable | mptg::kFromApi), seen(s, kT), scanned, true, 0, false),
     B::Pick::Transfer, false);
  is(B::choose(seen(s, kT, mptg::kUnreadable), seen(s, kT), software, true, 0, false), B::Pick::Path, true);
  // 6. A Scanned D record of an older parser, no T: path names, Pending.
  is(B::choose(none, seen(s, kT), scanned, false, 0, false), B::Pick::Path, true);
  is(B::choose(none, seen(s, kT), scanned, true, 0, false), B::Pick::Device, false);
  // The scan failed on it at this size and time: the path, not retried.
  is(B::choose(none, seen(s, kT), unreadable, true, 0, false), B::Pick::Path, false);
  // Without T, D's rows for files T covered turn Pending.
  is(B::choose(none, seen(s, kT), software, true, 0, false), B::Pick::Path, true);
  // No walk since the commit: T's listing stands for the walk.
  is(B::choose(seen(s + 1, kT), seen(s, kT), software, true, 0, true), B::Pick::Transfer, false);
  is(B::choose(seen(s, kT), none, B::Row{}, true, 0, true), B::Pick::Transfer, false);
  // (7., the walk's Acme against T's ACME, is the merge's: below.)
}

void test_names_match_by_their_bytes() {
  // 7. The walk's "Acme/x.mp3" against T's "ACME/x.mp3": no T match on the
  // device; the file is the scan's until the software takes the spelling.
  const std::vector<uint8_t> t = writeTags(mptg::kSourceTransfer, {{"ACME/x.mp3", 1000, kT, "Upper"}});
  const std::vector<uint8_t> d = writeTags(mptg::kSourceDevice, {{"Acme/x.mp3", 1000, kT}});
  cc::MemSource ts(t.data(), static_cast<uint32_t>(t.size())), ds(d.data(), static_cast<uint32_t>(d.size()));
  synthcard::SameRows rows(B::Status::Software);
  B::Config c;
  c.transfer = &ts;
  c.device = &ds;
  c.rows = &rows;
  LibraryIndex idx(Heap::alloc, Heap::release, Heap::shrink);
  B builder(Heap::alloc, Heap::release);
  const B::Result r = builder.build(idx, c);
  TEST_ASSERT_TRUE(r.built);
  TEST_ASSERT_EQUAL_UINT32(1, idx.trackCount());
  TEST_ASSERT_EQUAL_UINT32(1, r.ignored);
  TEST_ASSERT_EQUAL_UINT32(1, r.pending);
  const uint32_t id = findPath(idx, "Acme/x.mp3");
  TEST_ASSERT_NOT_EQUAL(LibraryIndex::kNone, id);
  TEST_ASSERT_EQUAL_STRING("x", title(idx, id).c_str());
  TEST_ASSERT_TRUE(idx.track(id).flags & LibraryIndex::kTrackPending);
  TEST_ASSERT_EQUAL_UINT32(LibraryIndex::kNone, findPath(idx, "ACME/x.mp3"));
}

// ---- the shared fixtures through the builder ----

void test_fixture_transfer_and_device() {
  const std::string t = bytesOf("golden/tags-0000002a.bin");
  const std::string d = bytesOf("golden/tags-device.bin");
  TEST_ASSERT_TRUE(!t.empty() && !d.empty());
  cc::MemSource ts(t.data(), static_cast<uint32_t>(t.size())), ds(d.data(), static_cast<uint32_t>(d.size()));
  LibraryIndex idx(Heap::alloc, Heap::release, Heap::shrink);
  B builder(Heap::alloc, Heap::release);
  B::Config c;
  c.transfer = &ts;
  c.device = &ds;
  c.transferLists = true;  // no walk since the commit: T's files are there
  B::Result r = builder.build(idx, c);
  TEST_ASSERT_TRUE(r.built);
  TEST_ASSERT_TRUE(r.transferUsed && r.deviceUsed && !r.restarted);
  // T's 14 audio files (its cover.jpg isn't a track) and D's 4.
  TEST_ASSERT_EQUAL_UINT32(18, idx.trackCount());
  TEST_ASSERT_EQUAL_UINT32(13, r.fromTransfer);  // Artist/x.mp3 is UNREADABLE: the path
  TEST_ASSERT_EQUAL_UINT32(3, r.fromDevice);     // broken.mp3 is UNREADABLE: the path
  TEST_ASSERT_EQUAL_UINT32(2, r.fromPath);
  TEST_ASSERT_EQUAL_UINT32(1, r.pending);
  TEST_ASSERT_EQUAL_UINT32(cc::get32(reinterpret_cast<const uint8_t*>(d.data()) + 32), r.deviceCrc);

  // The album folder Artist/Album, from T's records: elected name, year,
  // line and discs; by disc, then folder, then number.
  const uint32_t al = findAlbumByFolder(idx, "/music/Artist/Album");
  TEST_ASSERT_NOT_EQUAL(LibraryIndex::kNone, al);
  TEST_ASSERT_EQUAL_STRING("Album", idx.albumName(al));
  TEST_ASSERT_EQUAL_UINT16(2019, idx.album(al).year);
  TEST_ASSERT_EQUAL_STRING("Lantern Choir", idx.albumArtistLine(al));
  TEST_ASSERT_EQUAL_UINT8(2, idx.album(al).discs);
  TEST_ASSERT_TRUE(idx.album(al).flags & LibraryIndex::kTagged);
  const LibraryIndex::Span tr = idx.tracksOfAlbum(al);
  const char* order[] = {"01 - Title.mp3", "02 - Other.mp3", "01 - Disc One.flac", "01 - Disc Two.opus"};
  TEST_ASSERT_EQUAL_UINT32(4, tr.count);
  for (uint32_t i = 0; i < tr.count; ++i) TEST_ASSERT_EQUAL_STRING(order[i], idx.trackFileName(tr[i]));
  // The records' titles: a slice of the name, or a string of its own ("A B",
  // stored with the TAB as a space).
  TEST_ASSERT_EQUAL_STRING("Title", title(idx, tr[0]).c_str());
  TEST_ASSERT_EQUAL_STRING("A B", title(idx, tr[1]).c_str());
  TEST_ASSERT_EQUAL_UINT32(LibraryIndex::kFromTransfer, source(idx, tr[1]));
  // The track artists: shown where they differ from the album's line.
  TEST_ASSERT_EQUAL_STRING("Lantern Choir, Guest Voice", idx.trackArtistName(tr[0]));
  TEST_ASSERT_EQUAL_STRING("X, Y", idx.trackArtistName(tr[1]));
  TEST_ASSERT_EQUAL_UINT32(LibraryIndex::kNone, idx.track(tr[2]).trackArtist);
  TEST_ASSERT_EQUAL_STRING("Lantern Choir", idx.trackArtistName(tr[2]));
  TEST_ASSERT_TRUE(idx.track(tr[0]).flags & LibraryIndex::kTrackJpeg);
  TEST_ASSERT_FALSE(idx.track(tr[2]).flags & LibraryIndex::kTrackJpeg);  // a PNG
  TEST_ASSERT_TRUE(idx.track(tr[2]).flags & LibraryIndex::kTrackCompilation);
  TEST_ASSERT_EQUAL_UINT16(180, idx.track(tr[0]).durationS);
  // The artist folder keeps its name: "Lantern Choir" isn't "Artist".
  TEST_ASSERT_EQUAL_STRING("Artist", idx.artistName(idx.album(al).artist));

  // UNREADABLE without FROM_API: named from its path, and Pending.
  const uint32_t x = findPath(idx, "Artist/x.mp3");
  TEST_ASSERT_EQUAL_STRING("x", title(idx, x).c_str());
  TEST_ASSERT_EQUAL_UINT32(LibraryIndex::kFromPath, source(idx, x));
  TEST_ASSERT_TRUE(idx.track(x).flags & LibraryIndex::kTrackPending);
  // NO_TAGS: the record is chosen, its names come from the path.
  const uint32_t ax = findPath(idx, "A/x.mp3");
  TEST_ASSERT_EQUAL_STRING("x", title(idx, ax).c_str());
  TEST_ASSERT_EQUAL_UINT32(LibraryIndex::kFromTransfer, source(idx, ax));
  TEST_ASSERT_EQUAL_UINT16(186, idx.track(ax).durationS);
  // FROM_API: used like any other; its loose album keeps "" and no year.
  const uint32_t y = findPath(idx, "A B/y.mp3");
  TEST_ASSERT_EQUAL_STRING("Why", title(idx, y).c_str());
  TEST_ASSERT_EQUAL_UINT16(3, idx.track(y).number);
  TEST_ASSERT_EQUAL_STRING("", idx.albumName(idx.track(y).album));
  TEST_ASSERT_TRUE(idx.album(idx.track(y).album).flags & LibraryIndex::kLoose);
  TEST_ASSERT_EQUAL_UINT16(0, idx.album(idx.track(y).album).year);
  TEST_ASSERT_EQUAL_STRING("Some Band, Other Band", idx.albumArtistLine(idx.track(y).album));
  // The NFC and NFD folders stay two folders, two artists.
  TEST_ASSERT_NOT_EQUAL(LibraryIndex::kNone, findPath(idx, "Caf\xC3\xA9/Album/01 - Title.mp3"));
  TEST_ASSERT_NOT_EQUAL(LibraryIndex::kNone, findPath(idx, "Cafe\xCC\x81/Album/01 - Title.mp3"));
  // Control characters became spaces in the record (2.3.6).
  TEST_ASSERT_EQUAL_STRING("Del Name, Unit Sep", idx.trackArtistName(findPath(idx, "\xC3\x89/u.mp3")));

  // D's records: the hand-copied album.
  const uint32_t first = findPath(idx, "Hand Copied/Album/01 - First.flac");
  TEST_ASSERT_EQUAL_UINT32(LibraryIndex::kFromDevice, source(idx, first));
  TEST_ASSERT_EQUAL_STRING("First", title(idx, first).c_str());
  TEST_ASSERT_EQUAL_STRING("Quiet Harbour", idx.albumArtistLine(idx.track(first).album));
  TEST_ASSERT_EQUAL_STRING("Hand Copied", idx.artistName(idx.track(first).artist));
  const uint32_t broken = findPath(idx, "Hand Copied/broken.mp3");
  TEST_ASSERT_EQUAL_UINT32(LibraryIndex::kFromPath, source(idx, broken));
  TEST_ASSERT_FALSE(idx.track(broken).flags & LibraryIndex::kTrackPending);  // not retried

  // After the first walk (D is the listing), T's files D doesn't list are
  // left out: D lists only its 4.
  LibraryIndex walked(Heap::alloc, Heap::release, Heap::shrink);
  c.transferLists = false;
  r = builder.build(walked, c);
  TEST_ASSERT_TRUE(r.built);
  TEST_ASSERT_EQUAL_UINT32(4, walked.trackCount());
  TEST_ASSERT_EQUAL_UINT32(14, r.ignored);
}

// A bad T, found at its end (a CRC), restarts the build from D alone; a T
// that is absent from the start isn't a restart. A bad D: T's listing alone.
void test_restarts() {
  const std::string tGood = bytesOf("golden/tags-0000002a.bin");
  const std::string d = bytesOf("golden/tags-device.bin");
  std::string tBad = tGood;
  const size_t at = tBad.find("Guest Voice");
  TEST_ASSERT_TRUE(at != std::string::npos);
  tBad[at] = 'Q';  // inside STRS: only its CRC can tell, at the end
  LibraryIndex idx(Heap::alloc, Heap::release, Heap::shrink);
  B builder(Heap::alloc, Heap::release);
  {
    cc::MemSource ts(tBad.data(), static_cast<uint32_t>(tBad.size())), ds(d.data(), static_cast<uint32_t>(d.size()));
    B::Config c;
    c.transfer = &ts;
    c.device = &ds;
    c.transferLists = true;
    const B::Result r = builder.build(idx, c);
    TEST_ASSERT_TRUE(r.built);
    TEST_ASSERT_TRUE(r.restarted);
    TEST_ASSERT_FALSE(r.transferUsed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cc::Why::SectionCrc), static_cast<int>(r.transferWhy));
    TEST_ASSERT_EQUAL_UINT32(4, idx.trackCount());  // D's alone
    TEST_ASSERT_EQUAL_UINT32(0, r.fromTransfer);
  }
  {
    std::string notT = tGood;
    notT[0] = 'X';
    cc::MemSource ts(notT.data(), static_cast<uint32_t>(notT.size())), ds(d.data(), static_cast<uint32_t>(d.size()));
    B::Config c;
    c.transfer = &ts;
    c.device = &ds;
    const B::Result r = builder.build(idx, c);
    TEST_ASSERT_TRUE(r.built && !r.restarted && !r.transferUsed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cc::Why::Magic), static_cast<int>(r.transferWhy));
    TEST_ASSERT_EQUAL_UINT32(4, idx.trackCount());
  }
  {
    std::string dBad = d;
    const size_t k = dBad.find("Quiet Harbour");
    dBad[k] = 'q';
    cc::MemSource ts(tGood.data(), static_cast<uint32_t>(tGood.size())),
        ds(dBad.data(), static_cast<uint32_t>(dBad.size()));
    B::Config c;
    c.transfer = &ts;
    c.device = &ds;
    const B::Result r = builder.build(idx, c);  // D is the listing, until it fails
    TEST_ASSERT_TRUE(r.built && r.restarted && r.transferUsed && !r.deviceUsed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cc::Why::SectionCrc), static_cast<int>(r.deviceWhy));
    TEST_ASSERT_EQUAL_UINT32(14, idx.trackCount());  // T's listing alone
  }
  {
    // Neither: the caller walks.
    std::string notT = tGood, notD = d;
    notT[0] = notD[0] = 'X';
    cc::MemSource ts(notT.data(), static_cast<uint32_t>(notT.size())), ds(notD.data(), static_cast<uint32_t>(notD.size()));
    B::Config c;
    c.transfer = &ts;
    c.device = &ds;
    const B::Result r = builder.build(idx, c);
    TEST_ASSERT_FALSE(r.built);
    TEST_ASSERT_TRUE(r.noRecords);
    TEST_ASSERT_FALSE(idx.ready());
  }
  TEST_ASSERT_EQUAL_size_t(idx.memory().total, Heap::live);  // the builder gave its memory back
}

// ---- the merge on the synthetic library ----

void test_skew_confirmations_and_ghosts() {
  const synth::Spec spec = synth::userShape(400);
  const synthcard::Card card = synthcard::make(spec);
  // T recorded every stamp an hour early (a PC in another time zone); D's
  // rows are the walk's.
  synthcard::Write tw;
  tw.shift = -3600;
  const std::vector<uint8_t> t = synthcard::tagsFile(card, tw);
  synthcard::Write dw;
  dw.source = mptg::kSourceDevice;
  dw.fullRecords = false;
  const std::vector<uint8_t> d = synthcard::tagsFile(card, dw);
  TEST_ASSERT_FALSE(t.empty() || d.empty());
  cc::MemSource ts(t.data(), static_cast<uint32_t>(t.size())), ds(d.data(), static_cast<uint32_t>(d.size()));
  synthcard::SameRows software(B::Status::Software);
  B builder(Heap::alloc, Heap::release);
  LibraryIndex idx(Heap::alloc, Heap::release, Heap::shrink);
  B::Config c;
  c.transfer = &ts;
  c.device = &ds;
  c.rows = &software;
  c.skew = 3600;
  B::Result r = builder.build(idx, c);
  TEST_ASSERT_TRUE(r.built);
  TEST_ASSERT_EQUAL_UINT32(card.files.size(), r.fromTransfer);
  TEST_ASSERT_EQUAL_UINT32(card.files.size(), software.asked);
  // No skew: every stamp doubtful, none confirmed: path names, all Pending.
  c.skew = 0;
  r = builder.build(idx, c);
  TEST_ASSERT_EQUAL_UINT32(0, r.fromTransfer);
  TEST_ASSERT_EQUAL_UINT32(card.files.size(), r.pending);
  // Confirmed by qfp (the walk's checks, saved in D): T.
  synthcard::SameRows confirmed(B::Status::Software, true);
  c.rows = &confirmed;
  r = builder.build(idx, c);
  TEST_ASSERT_EQUAL_UINT32(card.files.size(), r.fromTransfer);

  // A new commit dropped a file: before the first walk, D's row for it (a
  // Software row) is a ghost and goes; after it, the walk had it, so it
  // stays, path-named and Pending.
  synthcard::Card fewer = card;
  const std::string gone = fewer.files.back().rel;
  fewer.files.pop_back();
  synthcard::Write exact;
  const std::vector<uint8_t> t2 = synthcard::tagsFile(fewer, exact);
  cc::MemSource ts2(t2.data(), static_cast<uint32_t>(t2.size()));
  c.transfer = &ts2;
  c.rows = &software;
  c.transferLists = true;
  r = builder.build(idx, c);
  TEST_ASSERT_EQUAL_UINT32(1, r.dropped);
  TEST_ASSERT_EQUAL_UINT32(fewer.files.size(), idx.trackCount());
  TEST_ASSERT_EQUAL_UINT32(LibraryIndex::kNone, findPath(idx, gone.c_str()));
  c.transferLists = false;
  r = builder.build(idx, c);
  TEST_ASSERT_EQUAL_UINT32(0, r.dropped);
  TEST_ASSERT_EQUAL_UINT32(card.files.size(), idx.trackCount());
  const uint32_t g = findPath(idx, gone.c_str());
  TEST_ASSERT_TRUE(idx.track(g).flags & LibraryIndex::kTrackPending);
}

// An older parser's records are read again: with a builder that wants a
// newer one, D's records name nothing and every file is Pending.
void test_older_device_parser() {
  const synth::Spec spec = synth::userShape(120);
  const synthcard::Card card = synthcard::make(spec);
  synthcard::Write dw;
  dw.source = mptg::kSourceDevice;
  dw.parserVersion = 2;
  const std::vector<uint8_t> d = synthcard::tagsFile(card, dw);
  cc::MemSource ds(d.data(), static_cast<uint32_t>(d.size()));
  B builder(Heap::alloc, Heap::release);
  LibraryIndex idx(Heap::alloc, Heap::release, Heap::shrink);
  B::Config c;
  c.device = &ds;
  c.minParser = 2;
  B::Result r = builder.build(idx, c);
  TEST_ASSERT_EQUAL_UINT32(card.files.size(), r.fromDevice);
  c.minParser = 3;
  r = builder.build(idx, c);
  TEST_ASSERT_EQUAL_UINT32(0, r.fromDevice);
  TEST_ASSERT_EQUAL_UINT32(card.files.size(), r.pending);
}

// The folders' facts are asked once per folder entered, and give the
// covers; THUMB album folders get the transfer thumbnail unless a cover the
// transfer doesn't own is there.
void test_facts_and_thumbnails() {
  const synth::Spec spec = synth::userShape(300);
  const synthcard::Card card = synthcard::make(spec);
  synthcard::Write tw;
  tw.thumbs = true;
  tw.ownedCovers = true;
  const std::vector<uint8_t> t = synthcard::tagsFile(card, tw);
  cc::MemSource ts(t.data(), static_cast<uint32_t>(t.size()));
  B builder(Heap::alloc, Heap::release);
  for (const bool owned : {true, false}) {
    synthcard::Facts facts(card, owned);
    LibraryIndex idx(Heap::alloc, Heap::release, Heap::shrink);
    B::Config c;
    c.transfer = &ts;
    c.facts = &facts;
    const B::Result r = builder.build(idx, c);
    TEST_ASSERT_TRUE(r.built);
    TEST_ASSERT_EQUAL_UINT32(card.files.size(), idx.trackCount());
    // Once per folder of the index (the root, the artists, the albums, the discs).
    TEST_ASSERT_EQUAL_UINT32(idx.folderCount(), facts.asked);
    uint32_t covers = 0, thumbs = 0;
    for (uint32_t a = 0; a < idx.albumCount(); ++a) {
      const bool hasCover = idx.albumCover(a) != LibraryIndex::kNone;
      covers += hasCover;
      const bool thumb = (idx.album(a).flags & LibraryIndex::kTransferThumb) != 0;
      thumbs += thumb;
      // An owned cover doesn't beat the thumbnail; the listener's does.
      TEST_ASSERT_EQUAL(owned || !hasCover, thumb);
    }
    uint32_t want = 0;
    for (size_t a = 0; a < card.albumCovers.size(); ++a) want += card.albumCovers[a];
    TEST_ASSERT_EQUAL_UINT32(want, covers);
    TEST_ASSERT_EQUAL_UINT32(owned ? idx.albumCount() : idx.albumCount() - want, thumbs);
    char buf[300];
    for (uint32_t a = 0; a < idx.albumCount(); ++a) {
      if (idx.albumCover(a) == LibraryIndex::kNone) continue;
      TEST_ASSERT_TRUE(idx.imagePath(idx.albumCover(a), buf, sizeof(buf)) > 0);
      TEST_ASSERT_TRUE(std::strstr(buf, "/cover.jpg") != nullptr);
      break;
    }
  }
  // Without facts, before the first walk, T's own cover.jpg records give the
  // images (owned: the thumbnail still shows).
  {
    LibraryIndex idx(Heap::alloc, Heap::release, Heap::shrink);
    B::Config c;
    c.transfer = &ts;
    const B::Result r = builder.build(idx, c);
    TEST_ASSERT_TRUE(r.built);
    uint32_t covers = 0;
    for (uint32_t a = 0; a < idx.albumCount(); ++a) {
      covers += idx.albumCover(a) != LibraryIndex::kNone;
      TEST_ASSERT_TRUE(idx.album(a).flags & LibraryIndex::kTransferThumb);
    }
    TEST_ASSERT_TRUE(covers > 0);
  }
}

// ---- the 20k measurements ----

namespace {
struct Built {
  std::vector<uint8_t> bytes;
  LibraryIndex::Memory memory;
  size_t peak = 0;  // the heap's: the index's blocks and the builder's own
  uint32_t tracks = 0, albums = 0, artists = 0;
  std::vector<uint16_t> lengths;  // each track's durationS, by id
  B::Result result;
  long ms = 0;
};

// How a build is made beyond its source.
struct Extra {
  bool lists = false;    // T lists: no walk since its commit (the boot after a transfer)
  bool counts = true;    // D's statuses say how much of D is its own (DSTA's header, N4)
  int32_t deviceMs = 0;  // ms added to D's lengths (the device trims an MP3's encoder delay)
  bool keepLengths = true;
};

Built buildFrom(const synthcard::Card& card, bool fromTransfer, bool keepSources = false, const Extra& x = Extra()) {
  Built out;
  synthcard::Write tw;
  synthcard::Write dw;
  dw.source = mptg::kSourceDevice;
  dw.fullRecords = !fromTransfer;  // T's files: D's Software rows; else D's own records
  dw.lengthMs = x.deviceMs;
  const std::vector<uint8_t> t = fromTransfer ? synthcard::tagsFile(card, tw) : std::vector<uint8_t>{};
  const std::vector<uint8_t> d = synthcard::tagsFile(card, dw);
  cc::MemSource ts(t.data(), static_cast<uint32_t>(t.size())), ds(d.data(), static_cast<uint32_t>(d.size()));
  synthcard::Facts facts(card, false);
  synthcard::SameRows rows(fromTransfer ? B::Status::Software : B::Status::Scanned, false, x.counts);
  Heap::reset();
  {
    LibraryIndex idx(Heap::alloc, Heap::release, Heap::shrink);
    B builder(Heap::alloc, Heap::release);
    B::Config c;
    c.transfer = fromTransfer ? &ts : nullptr;
    c.transferLists = x.lists;
    c.device = &ds;
    c.rows = &rows;
    c.facts = &facts;
    const auto t0 = std::chrono::steady_clock::now();
    out.result = builder.build(idx, c);
    out.ms = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    out.peak = Heap::peak;
    out.memory = idx.memory();
    out.tracks = idx.trackCount();
    out.albums = idx.albumCount();
    out.artists = idx.artistCount();
    for (uint32_t i = 0; i < idx.trackCount(); ++i) out.lengths.push_back(idx.track(i).durationS);
    if (!keepSources) idx.forgetSources();
    if (!x.keepLengths) idx.forgetLengths();
    MemorySink file;
    LibraryIndex::Inputs in;
    in.cardId = 1;
    idx.save(file, in);
    out.bytes.assign(file.data(), file.data() + file.size());
    TEST_ASSERT_EQUAL_size_t(Heap::live, out.memory.total);
  }
  TEST_ASSERT_EQUAL_size_t(0, Heap::live);
  return out;
}
}  // namespace

// The budget (3.4.3, 3.5): library.idx v6 at 20k about 1.75-1.85 MB, 88-92
// B a track; the build peak, pre-sized, about 2.0 MB (the index's blocks,
// its votes and sorts, and the builder's walkers). Measured on the synthetic
// library in the user's shape, with tags at the measured rates and names at
// the measured lengths: 88.6 B a track, a peak of 1.98 MB.
constexpr size_t kIndexBytesPerTrack = 92;
constexpr size_t kBuildPeak20k = 2050u * 1000u;

void test_20k_memory_and_peak() {
  const synth::Spec spec = synth::userShape(20000);
  const synthcard::Card card = synthcard::make(spec);
  TEST_ASSERT_EQUAL_size_t(20000, card.files.size());
  {
    // The synthetic names are as long as the measured library's, on
    // average (its database statistics: file names 30.4 bytes, titles 16.9,
    // albums 16.6, artists 14.9), so the bytes a track costs are its.
    double leaf = 0, titles = 0, albums = 0, artists = 0;
    uint32_t nTitles = 0, nAlbums = 0, nArtists = 0;
    for (const auto& f : card.files) {
      leaf += static_cast<double>(f.rel.size() - f.rel.rfind('/') - 1);
      if (!f.title.empty()) titles += static_cast<double>(f.title.size()), ++nTitles;
      if (!f.album.empty()) albums += static_cast<double>(f.album.size()), ++nAlbums;
      const size_t sep = f.artist.find('\x1F');
      if (!f.artist.empty()) artists += static_cast<double>(sep == std::string::npos ? f.artist.size() : sep), ++nArtists;
    }
    leaf /= card.files.size();
    titles /= nTitles;
    albums /= nAlbums;
    artists /= nArtists;
    printf("[builder] synthetic names: file names %.1f bytes, titles %.1f, albums %.1f, artists %.1f\n", leaf, titles,
           albums, artists);
    auto near = [](double got, double want) { return got > want * 0.85 && got < want * 1.15; };
    TEST_ASSERT_TRUE(near(leaf, 30.4));
    TEST_ASSERT_TRUE(near(titles, 16.9));
    TEST_ASSERT_TRUE(near(albums, 16.6));
    TEST_ASSERT_TRUE(near(artists, 14.9));
  }
  const Built t = buildFrom(card, true);
  TEST_ASSERT_TRUE(t.result.built);
  TEST_ASSERT_EQUAL_UINT32(20000, t.tracks);
  TEST_ASSERT_EQUAL_UINT32(20000, t.result.fromTransfer);
  TEST_ASSERT_EQUAL_UINT32(spec.albums, t.albums);    // keyed by folder: one each
  TEST_ASSERT_EQUAL_UINT32(spec.artists, t.artists);
  printf("[builder] 20000 tracks from T: %ld ms; index %u bytes (%.1f B/track): strings %u, tracks %u, artists %u, "
         "albums %u, folders %u, views %u; build peak %u (index %u, the builder's own %u)\n",
         t.ms, static_cast<unsigned>(t.memory.total), t.memory.total / 20000.0,
         static_cast<unsigned>(t.memory.strings), static_cast<unsigned>(t.memory.tracks),
         static_cast<unsigned>(t.memory.artists), static_cast<unsigned>(t.memory.albums),
         static_cast<unsigned>(t.memory.folders), static_cast<unsigned>(t.memory.views), static_cast<unsigned>(t.peak),
         static_cast<unsigned>(t.memory.buildPeak), static_cast<unsigned>(t.result.workBytes));
  TEST_ASSERT_TRUE_MESSAGE(t.memory.total <= kIndexBytesPerTrack * 20000, "the index over 92 B a track");
  TEST_ASSERT_TRUE_MESSAGE(t.peak <= kBuildPeak20k, "the build peak over its budget");
  TEST_ASSERT_TRUE(t.memory.buildPeak <= t.peak);

  // The boot after a transfer (3.2.2: the transfer's identity differs): no
  // walk since the commit, so T lists, and D still has a Software row for
  // every file T lists (review of N2). Sized from T and D's own rows (none
  // here), the build stays in the same budget and makes the same index;
  // sized from both headers, it would hold its blocks twice as big.
  Extra lists;
  lists.lists = true;
  const Built l = buildFrom(card, true, false, lists);
  TEST_ASSERT_TRUE(l.result.built);
  TEST_ASSERT_EQUAL_UINT32(20000, l.result.fromTransfer);
  TEST_ASSERT_EQUAL_UINT32(0, l.result.dropped);
  printf("[builder] 20000 tracks from T listing, D's Software rows for them: build peak %u\n",
         static_cast<unsigned>(l.peak));
  TEST_ASSERT_TRUE_MESSAGE(l.peak <= kBuildPeak20k, "the build peak after a transfer over its budget");
  TEST_ASSERT_TRUE(l.bytes == buildFrom(card, true).bytes);
  Extra blind = lists;
  blind.counts = false;
  const Built b = buildFrom(card, true, false, blind);
  printf("[builder] the same, D's own counts unknown (sized from both headers): build peak %u\n",
         static_cast<unsigned>(b.peak));
  TEST_ASSERT_TRUE(b.peak > l.peak + 500000);
  TEST_ASSERT_TRUE(b.bytes == l.bytes);
}

// The same files from T and from D build the same index (2.17, item 5: the
// research's M7 test on the host), byte for byte once the tracks' sources
// (which differ by definition) are cleared, when the two records' lengths
// are equal. Real ones may differ by up to 100 ms (2.17, item 3: the
// software's reader keeps an MP3's encoder delay, the device trims it), so
// a track's length in whole seconds may differ by one (review of N2): with
// D's lengths 50 ms shorter, every length is within a second of T's, some
// differ, and the rest is still the same bytes.
void test_transfer_and_device_build_the_same_index() {
  const synth::Spec spec = synth::userShape(3000);
  const synthcard::Card card = synthcard::make(spec);
  const Built t = buildFrom(card, true);
  const Built d = buildFrom(card, false);
  TEST_ASSERT_EQUAL_UINT32(card.files.size(), t.result.fromTransfer);
  TEST_ASSERT_EQUAL_UINT32(card.files.size(), d.result.fromDevice);
  TEST_ASSERT_EQUAL_size_t(t.bytes.size(), d.bytes.size());
  TEST_ASSERT_TRUE(t.bytes == d.bytes);
  // With the sources kept, they differ (each track says where its names are from).
  const Built ts = buildFrom(card, true, true);
  const Built ds = buildFrom(card, false, true);
  TEST_ASSERT_FALSE(ts.bytes == ds.bytes);
  TEST_ASSERT_EQUAL_size_t(ts.bytes.size(), ds.bytes.size());
  // The device's lengths 50 ms shorter.
  Extra trimmed;
  trimmed.deviceMs = -50;
  const Built dt = buildFrom(card, false, false, trimmed);
  TEST_ASSERT_EQUAL_UINT32(t.lengths.size(), dt.lengths.size());
  uint32_t differ = 0;
  for (size_t i = 0; i < t.lengths.size(); ++i) {
    const int gap = static_cast<int>(t.lengths[i]) - static_cast<int>(dt.lengths[i]);
    TEST_ASSERT_TRUE(gap == 0 || gap == 1);
    differ += gap ? 1 : 0;
  }
  printf("[builder] D's lengths 50 ms shorter: %u of %u tracks a second shorter\n", static_cast<unsigned>(differ),
         static_cast<unsigned>(t.lengths.size()));
  TEST_ASSERT_TRUE(differ > 0);
  TEST_ASSERT_FALSE(t.bytes == dt.bytes);
  Extra noLengths;
  noLengths.keepLengths = false;
  trimmed.keepLengths = false;
  TEST_ASSERT_TRUE(buildFrom(card, true, false, noLengths).bytes == buildFrom(card, false, false, trimmed).bytes);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_precedence_vectors);
  RUN_TEST(test_names_match_by_their_bytes);
  RUN_TEST(test_fixture_transfer_and_device);
  RUN_TEST(test_restarts);
  RUN_TEST(test_skew_confirmations_and_ghosts);
  RUN_TEST(test_older_device_parser);
  RUN_TEST(test_facts_and_thumbnails);
  RUN_TEST(test_20k_memory_and_peak);
  RUN_TEST(test_transfer_and_device_build_the_same_index);
  return UNITY_END();
}
