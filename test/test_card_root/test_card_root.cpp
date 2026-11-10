// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for what cardroot::read() says when the card fails a read
// (lib/core/CardRoot.h, docs/METADATA.md 3.2.2): that is never "no transfer
// data". Over the golden root and T, on a card in memory and on the real FatFs
// with a RAM disk under it: each read and open failed in its turn, a candidate
// that can't be read holding up the election; the other way about, a file cut or
// damaged, its directory entry too, never reads as the card's fault, since a boot
// that took it so would keep a dead root for ever; a card with no root or no
// /.mstream says Missing; a Root read again; and the boot's reads, how many and with
// what waits. What the boot then does with it is test_library_update's. The boot's
// other two reads the same way, on the real FatFs: D and its journals
// (TagStore::open()), library.idx (LibraryUpdate::boot()), and their damaged directory
// entries. Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../support/FatModel.h"
#include "../support/CutFs.h"
#include "CardManifest.h"
#include "CardRoot.h"
#include "CardTags.h"
#include "LibraryUpdate.h"
#include "QueueFile.h"
// The firmware's own say on what is the card's fault (FatFs::openRead(),
// FatFile::damaged()), over the real FatFs.
#include "../../src/storage/CardFat.cpp"

namespace cc = cardcontract;
using cutfs::CutFs;
using cardroot::Root;

void setUp() {}
void tearDown() {}

namespace {

constexpr const char* kBin = "/.mstream/manifest.bin";
constexpr const char* kTmp = "/.mstream/manifest.tmp";
constexpr const char* kTags = "/.mstream/tags-0000002a.bin";

std::vector<uint8_t> golden(const char* name) {
  const std::string path = std::string("test/fixtures/card/golden/") + name;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::vector<uint8_t> out;
  uint8_t buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

// (Filled apart from the static's own initialiser: an assert that failed in one would
// leave its guard held, and the next call would wait on it for ever.)
const std::vector<uint8_t>& manifest() {
  static std::vector<uint8_t> b;
  if (b.empty()) b = golden("manifest.bin");
  return b;
}
const std::vector<uint8_t>& tags() {
  static std::vector<uint8_t> b;
  if (b.empty()) b = golden("tags-0000002a.bin");
  return b;
}

void fill(CutFs& fs, const std::vector<uint8_t>& root = manifest()) {
  fs.put(kBin, root);
  fs.put(kTags, tags());
}

struct Bytes : cc::Sink {
  bool write(uint32_t offset, const void* data, uint32_t n) override {
    if (offset + n > bytes.size()) bytes.resize(offset + n);
    if (n) std::memcpy(bytes.data() + offset, data, n);
    return true;
  }
  std::vector<uint8_t> bytes;
};

// A root naming the golden T, whose strings need more than one of the reader's
// stream windows (a third of the 1024 B every caller gives it). The golden root's
// fit in one, so a failed read among a root's strings needs this.
const std::vector<uint8_t>& longRoot() {
  static std::vector<uint8_t> root;
  if (!root.empty()) return root;
  root = [] {
    cc::MemSource src(tags().data(), static_cast<uint32_t>(tags().size()));
    cc::Container c;
    cc::mptg::Info info;
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(cc::mptg::openFile(c, src, &info)));
    cc::msmf::CompIn comp;
    comp.kind = cc::kMagicMptg;
    comp.generation = 42;
    comp.fileBytes = static_cast<uint32_t>(tags().size());
    comp.headerCrc = info.frame.headerCrc;
    std::vector<std::string> names;
    std::vector<const char*> ptrs;
    for (int i = 0; i < 6; ++i) {
      char head[16];
      std::snprintf(head, sizeof(head), "root%02d-", i);
      names.push_back(std::string(head) + std::string(64, 'x'));
    }
    for (const std::string& s : names) ptrs.push_back(s.c_str());
    cc::msmf::ManifestIn m;
    m.generation = 42;
    m.cardId = 0x1122334455667788ull;
    m.flags = cc::msmf::kFinal;
    m.commitId = 7;
    m.producer = "mstream-terminal test";
    m.comps = &comp;
    m.compCount = 1;
    m.roots = ptrs.data();
    m.rootCount = static_cast<uint32_t>(ptrs.size());
    Bytes out;
    const char* error = nullptr;
    if (!cc::msmf::write(out, m, nullptr, nullptr, &error)) TEST_FAIL_MESSAGE(error);
    return out.bytes;
  }();
  return root;
}

std::unique_ptr<Root> readRoot(tagstore::Fs& fs) {
  std::vector<uint8_t> scratch(1024);
  std::unique_ptr<Root> r(new Root());
  cardroot::read(fs, scratch.data(), static_cast<uint32_t>(scratch.size()), r.get());
  return r;
}

void arm(CutFs& fs, const char* prefix, long from, long to) {
  fs.faultPrefix = prefix;
  fs.faultEvents = 0;
  fs.faultFrom = from;
  fs.faultTo = to;
}

}  // namespace

// ---- the card in memory ----

void test_a_whole_card_reads_whole_and_says_no_fault() {
  CutFs fs;
  fill(fs);
  auto r = readRoot(fs);
  TEST_ASSERT_TRUE_MESSAGE(r->present, cc::whyName(r->why));
  TEST_ASSERT_TRUE(r->hasRoot);
  TEST_ASSERT_TRUE(r->why == cc::Why::Ok);
  TEST_ASSERT_FALSE(r->readFailed());
}

void test_no_root_is_missing_and_no_fault() {
  CutFs fs;
  auto r = readRoot(fs);
  TEST_ASSERT_FALSE(r->present);
  TEST_ASSERT_TRUE(r->why == cc::Why::Missing);
  TEST_ASSERT_FALSE(r->readFailed());
  char line[120];
  cardroot::describe(*r, line, sizeof(line));
  TEST_ASSERT_EQUAL_STRING("no transfer data (none on the card)", line);
  // A root whose T isn't on the card: its word, still.
  fs.put(kBin, manifest());
  r = readRoot(fs);
  TEST_ASSERT_TRUE(r->hasRoot);
  TEST_ASSERT_FALSE(r->present);
  TEST_ASSERT_TRUE(r->why == cc::Why::Missing);
  TEST_ASSERT_FALSE(r->readFailed());
}

// Each read and open of the card failed once, in its turn. The plan's are the
// status line's own, so a fault there leaves the root whole; so does one in ACKS,
// read on its own once the root is checked (the play log's: unread, it is none, and
// costs that boot its trim); any other leaves the root unread, and never reads as a
// card with no transfer data.
void test_each_read_failed_in_its_turn_is_the_cards_fault() {
  for (const std::vector<uint8_t>* root : {&manifest(), &longRoot()}) {
    CutFs fs;
    fill(fs, *root);
    arm(fs, "/.mstream/", 1000000, -1);  // (counted, none failed)
    TEST_ASSERT_TRUE(readRoot(fs)->present);
    const long events = fs.faultEvents;
    TEST_ASSERT_GREATER_THAN_INT32(4, events);
    int unread = 0, checkedNoName = 0, acksUnread = 0;
    for (long k = 0; k < events; ++k) {
      arm(fs, "/.mstream/", k, k + 1);
      auto r = readRoot(fs);
      char msg[80];
      std::snprintf(msg, sizeof(msg), "read %ld of %ld: %s", k, events, cc::whyName(r->why));
      TEST_ASSERT_TRUE_MESSAGE(r->present || r->readFailed(), msg);
      if (!r->present) {
        ++unread;
        TEST_ASSERT_TRUE_MESSAGE(r->why == cc::Why::Io, msg);
        TEST_ASSERT_FALSE_MESSAGE(r->autoDj.present || r->waves.present, msg);
        // The log line names T only once the root gave its name. (A read among the
        // root's strings fails after its check: it names the card, and no T yet.)
        char line[160];
        cardroot::describe(*r, line, sizeof(line));
        const bool named = r->tagsPath[0] != 0;
        TEST_ASSERT_TRUE_MESSAGE(std::strncmp(line, named ? "its T tags-" : "its root unread", named ? 11 : 15) == 0, line);
        if (r->hasRoot && !named) ++checkedNoName;
      } else if (r->acksWhy == cc::Why::Io) {
        ++acksUnread;
        TEST_ASSERT_FALSE_MESSAGE(r->acks.present, msg);
      }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(events - 1 - acksUnread, unread, "all but the plan's open and ACKS's reads");
    TEST_ASSERT_GREATER_THAN_INT(0, acksUnread);
    TEST_ASSERT_GREATER_THAN_INT(0, checkedNoName);
  }
}

void test_a_card_that_fails_every_read_or_is_gone_is_unread() {
  CutFs fs;
  fill(fs);
  arm(fs, "/.mstream/", 0, -1);
  TEST_ASSERT_TRUE(readRoot(fs)->readFailed());
  // T's reads alone: the root names the card, and T is unread, not missing.
  arm(fs, kTags, 0, -1);
  auto r = readRoot(fs);
  TEST_ASSERT_TRUE(r->hasRoot);
  TEST_ASSERT_TRUE(r->readFailed());
  fs.faultFrom = -1;
  fs.dead = true;
  TEST_ASSERT_TRUE(readRoot(fs)->readFailed());
  fs.dead = false;
  TEST_ASSERT_TRUE(readRoot(fs)->present);
}

void test_a_candidate_that_cant_be_read_holds_up_the_election() {
  CutFs fs;
  fill(fs);
  fs.put(kTmp, manifest());  // (a cut rename: the one commit twice)
  TEST_ASSERT_TRUE(readRoot(fs)->present);
  // Either one unread: it may be the newer, so no root is taken from the other.
  for (const char* unread : {kBin, kTmp}) {
    arm(fs, unread, 0, -1);
    auto r = readRoot(fs);
    TEST_ASSERT_TRUE_MESSAGE(r->readFailed(), unread);
    TEST_ASSERT_FALSE_MESSAGE(r->hasRoot, unread);
  }
  // No .tmp on the card, and its open failing: the same, it can't be told.
  fs.remove(kTmp);
  arm(fs, kTmp, 0, -1);
  TEST_ASSERT_TRUE(readRoot(fs)->readFailed());
}

// The other way about, every read answered: a boot that took a damaged root for a
// fault would keep it for ever. So a file cut at any length, or with any byte
// changed, is the file's fault.
void test_a_cut_or_damaged_file_is_never_the_cards_fault() {
  for (const char* path : {kBin, kTags}) {
    const std::vector<uint8_t> whole = path == kBin ? manifest() : tags();
    CutFs fs;
    fill(fs);
    for (size_t len = 0; len < whole.size(); ++len) {
      fs.put(path, std::vector<uint8_t>(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(len)));
      auto r = readRoot(fs);
      char msg[96];
      std::snprintf(msg, sizeof(msg), "%s cut to %lu bytes: %s", path, (unsigned long)len, cc::whyName(r->why));
      TEST_ASSERT_FALSE_MESSAGE(r->present, msg);
      TEST_ASSERT_FALSE_MESSAGE(r->readFailed(), msg);
    }
    const size_t head = std::min<size_t>(whole.size(), 1024);
    for (size_t at = 0; at < head; ++at) {
      std::vector<uint8_t> bad = whole;
      bad[at] ^= 0xFF;
      fs.put(path, bad);
      auto r = readRoot(fs);
      char msg[96];
      std::snprintf(msg, sizeof(msg), "%s byte %lu changed: %s", path, (unsigned long)at, cc::whyName(r->why));
      TEST_ASSERT_FALSE_MESSAGE(r->readFailed(), msg);
    }
  }
}

void test_a_root_read_again_forgets_the_fault_and_what_it_held() {
  CutFs fs;
  fill(fs);
  std::vector<uint8_t> scratch(1024);
  std::unique_ptr<Root> r(new Root());
  const auto again = [&]() { cardroot::read(fs, scratch.data(), static_cast<uint32_t>(scratch.size()), r.get()); };
  again();
  TEST_ASSERT_TRUE(r->present);
  arm(fs, "/.mstream/", 0, -1);
  again();
  TEST_ASSERT_TRUE(r->readFailed());
  TEST_ASSERT_FALSE(r->hasRoot);
  TEST_ASSERT_FALSE(r->identity.present);
  fs.faultFrom = -1;
  again();
  TEST_ASSERT_TRUE(r->present);
  TEST_ASSERT_FALSE(r->readFailed());
}

void test_the_log_line_says_the_root_is_unread() {
  CutFs fs;
  fill(fs);
  arm(fs, "/.mstream/manifest", 0, -1);
  auto r = readRoot(fs);
  char line[160];
  cardroot::describe(*r, line, sizeof(line));
  TEST_ASSERT_EQUAL_STRING("its root unread (a read of the card failed)", line);
  // The plan's word beside it, as beside any other.
  fs.put("/.mstream/pending.bin", std::vector<uint8_t>{1, 2, 3});
  r = readRoot(fs);
  cardroot::describe(*r, line, sizeof(line));
  TEST_ASSERT_EQUAL_STRING("its root unread (a read of the card failed); a transfer's plan is on the card (it didn't "
                           "finish)",
                           line);
  // T's alone: the root was read, and names the card.
  fs.remove("/.mstream/pending.bin");
  arm(fs, kTags, 0, -1);
  r = readRoot(fs);
  TEST_ASSERT_TRUE(r->hasRoot);
  cardroot::describe(*r, line, sizeof(line));
  TEST_ASSERT_EQUAL_STRING("its T tags-0000002a.bin, generation 42, unread (a read of the card failed)", line);
}

// ---- the boot's reads ----

std::vector<uint32_t> g_waits;
CutFs* g_watched = nullptr;
std::vector<long> g_eventsAtWait;  // g_watched's reads when each wait came
void noteWait(uint32_t ms) {
  g_waits.push_back(ms);
  if (g_watched) g_eventsAtWait.push_back(g_watched->faultEvents);
}

uint32_t bootRead(tagstore::Fs& fs, Root* r) {
  g_waits.clear();
  std::vector<uint8_t> scratch(1024);
  return cardroot::readAtBoot(fs, scratch.data(), static_cast<uint32_t>(scratch.size()), r, noteWait);
}

void test_the_boot_reads_once_unless_the_card_failed() {
  CutFs fs;
  std::unique_ptr<Root> r(new Root());
  // No root: its word, the first time.
  TEST_ASSERT_EQUAL_UINT32(1, bootRead(fs, r.get()));
  TEST_ASSERT_EQUAL_size_t(0, g_waits.size());
  // A whole one.
  fill(fs);
  TEST_ASSERT_EQUAL_UINT32(1, bootRead(fs, r.get()));
  TEST_ASSERT_TRUE(r->present);
  TEST_ASSERT_EQUAL_size_t(0, g_waits.size());
  // A damaged one: its word too, not read again.
  std::vector<uint8_t> bad = manifest();
  bad[0] ^= 0xFF;
  fs.put(kBin, bad);
  TEST_ASSERT_EQUAL_UINT32(1, bootRead(fs, r.get()));
  TEST_ASSERT_FALSE(r->readFailed());
  TEST_ASSERT_EQUAL_size_t(0, g_waits.size());
}

void test_the_boot_reads_again_while_the_card_fails_and_keeps_what_it_finds() {
  // Failing for the first reads of the root, whichever of its reads that takes.
  for (uint32_t failing = 1; failing <= 3; ++failing) {
    CutFs fs;
    fill(fs);
    // Each whole read here has the plan's open, .bin's and .tmp's: the root's
    // files fail for as long as `failing` reads take, one fault a read.
    arm(fs, "/.mstream/manifest.bin", 0, static_cast<long>(failing));
    std::unique_ptr<Root> r(new Root());
    const uint32_t reads = bootRead(fs, r.get());
    TEST_ASSERT_EQUAL_UINT32(failing + 1, reads);
    TEST_ASSERT_TRUE(r->present);
    TEST_ASSERT_EQUAL_size_t(failing, g_waits.size());
    for (uint32_t i = 0; i < failing; ++i) TEST_ASSERT_EQUAL_UINT32(cardroot::kBootWaitsMs[i], g_waits[i]);
  }
}

void test_the_boot_gives_up_after_its_reads() {
  CutFs fs;
  fill(fs);
  arm(fs, "/.mstream/", 0, -1);
  std::unique_ptr<Root> r(new Root());
  TEST_ASSERT_EQUAL_UINT32(4, bootRead(fs, r.get()));
  TEST_ASSERT_TRUE(r->readFailed());
  TEST_ASSERT_EQUAL_size_t(3, g_waits.size());
  TEST_ASSERT_EQUAL_UINT32(100, g_waits[0]);
  TEST_ASSERT_EQUAL_UINT32(300, g_waits[1]);
  TEST_ASSERT_EQUAL_UINT32(1000, g_waits[2]);
}

void test_the_boot_waits_between_its_reads_and_not_after_the_last() {
  CutFs fs;
  fill(fs);
  arm(fs, "/.mstream/", 0, -1);
  g_watched = &fs;
  g_eventsAtWait.clear();
  std::unique_ptr<Root> r(new Root());
  TEST_ASSERT_EQUAL_UINT32(4, bootRead(fs, r.get()));
  g_watched = nullptr;
  const long perRead = fs.faultEvents / 4;
  TEST_ASSERT_GREATER_THAN_INT32(0, perRead);
  TEST_ASSERT_EQUAL_size_t(3, g_eventsAtWait.size());
  for (int i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_INT32((i + 1) * perRead, g_eventsAtWait[static_cast<size_t>(i)]);
}

// ---- the real FatFs over a RAM disk ----

// A 4 GB FAT32 card, its FatFs mounted anew for each read, so each reads the card
// from the boot sector up, nothing held.
struct Card {
  fatmodel::RamDisk disk{8388608u};
  inline static FATFS fatfs;  // static: a failed assertion jumps past ~Card, and FatFs keeps this pointer
  cardfat::FatFs fs;
  Card() {
    cardfat::setDrive(0);
    fatmodel::Drive& d = fatmodel::drive(0);
    d = fatmodel::Drive{};
    d.disk = &disk;
    TEST_ASSERT_EQUAL_INT(FR_OK, fatmodel::format(0, 16384));
    TEST_ASSERT_EQUAL_INT(FR_OK, fatmodel::mount(0, &fatfs));
  }
  ~Card() {
    f_mount(nullptr, "0:", 0);
    fatmodel::drive(0) = fatmodel::Drive{};
  }
  void put(const char* path, const std::vector<uint8_t>& bytes) {
    tagstore::File* f = fs.open(path, tagstore::Fs::Mode::Create);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    TEST_ASSERT_TRUE(f->write(0, bytes.data(), static_cast<uint32_t>(bytes.size())));
    TEST_ASSERT_TRUE(fs.close(f));
  }
  void remount() {
    f_mount(nullptr, "0:", 0);
    TEST_ASSERT_EQUAL_INT(FR_OK, fatmodel::mount(0, &fatfs));
  }
  // The short directory entry in `folder` (0: the root folder) whose name begins
  // with `name` gets `cluster` for its start, as a damaged directory is found: on
  // the disk, under FatFs, with a mount after it. With every read answered.
  void damageEntry(const char* folder, const char* name, uint32_t cluster) {
    uint32_t first = static_cast<uint32_t>(fatfs.dirbase);
    if (folder) {
      DIR d;
      TEST_ASSERT_EQUAL_INT(FR_OK, f_opendir(&d, fatmodel::path(0, folder).c_str()));
      first = static_cast<uint32_t>(fatfs.database) + (static_cast<uint32_t>(d.obj.sclust) - 2) * fatfs.csize;
      f_closedir(&d);
    } else {
      first = static_cast<uint32_t>(fatfs.database) + (first - 2) * fatfs.csize;  // (FAT32's root is a cluster chain)
    }
    uint8_t s[512];
    const size_t n = std::strlen(name);
    for (uint32_t k = 0; k < fatfs.csize; ++k) {
      disk.read(first + k, s, 1);
      for (uint32_t o = 0; o < 512; o += 32) {
        if (s[o] == 0 || (s[o + 11] & 0x3F) == 0x0F || std::memcmp(s + o, name, n) != 0) continue;
        s[o + 20] = static_cast<uint8_t>(cluster >> 16);
        s[o + 21] = static_cast<uint8_t>(cluster >> 24);
        s[o + 26] = static_cast<uint8_t>(cluster);
        s[o + 27] = static_cast<uint8_t>(cluster >> 8);
        disk.write(first + k, s, 1);
        remount();
        return;
      }
    }
    TEST_FAIL_MESSAGE(name);
  }
};

void test_on_fatfs_no_folder_and_no_file_are_its_word_not_a_fault() {
  Card c;
  auto r = readRoot(c.fs);  // (no /.mstream: FR_NO_PATH)
  TEST_ASSERT_FALSE(r->present);
  TEST_ASSERT_TRUE(r->why == cc::Why::Missing);
  TEST_ASSERT_FALSE(r->readFailed());
  TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir(fatmodel::path(0, ".mstream").c_str()));
  r = readRoot(c.fs);  // (/.mstream, no file: FR_NO_FILE)
  TEST_ASSERT_TRUE(r->why == cc::Why::Missing);
  TEST_ASSERT_FALSE(r->readFailed());
}

// FatFs reads the card, every read answered, and finds its records inconsistent
// (FR_INT_ERR): no read again changes that. A boot that took it for the card's
// fault would hold a whole root back for ever.
void test_on_fatfs_a_damaged_directory_entry_is_the_files_fault_not_the_cards() {
  for (const uint32_t cluster : {0u, 1u, 0x0FFFFFF0u}) {
    // A leftover .tmp's: it isn't valid, and the .bin stands.
    {
      Card c;
      TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir(fatmodel::path(0, ".mstream").c_str()));
      c.put(kBin, manifest());
      c.put(kTags, tags());
      c.put(kTmp, manifest());
      c.damageEntry(".mstream", "MANIFESTTMP", cluster);
      std::unique_ptr<Root> r(new Root());
      TEST_ASSERT_EQUAL_UINT32(1, bootRead(c.fs, r.get()));
      TEST_ASSERT_TRUE_MESSAGE(r->present, cc::whyName(r->why));
      TEST_ASSERT_FALSE(r->readFailed());
    }
    // The only root's: no root, its word.
    {
      Card c;
      TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir(fatmodel::path(0, ".mstream").c_str()));
      c.put(kBin, manifest());
      c.put(kTags, tags());
      c.damageEntry(".mstream", "MANIFESTBIN", cluster);
      std::unique_ptr<Root> r(new Root());
      TEST_ASSERT_EQUAL_UINT32(1, bootRead(c.fs, r.get()));
      TEST_ASSERT_FALSE(r->readFailed());
      TEST_ASSERT_FALSE(r->hasRoot);
      TEST_ASSERT_TRUE_MESSAGE(r->why == cc::Why::FatRecords, cc::whyName(r->why));
    }
    // T's: the root names the card, and T failed.
    {
      Card c;
      TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir(fatmodel::path(0, ".mstream").c_str()));
      c.put(kBin, manifest());
      c.put(kTags, tags());
      c.damageEntry(".mstream", "TAGS-", cluster);
      std::unique_ptr<Root> r(new Root());
      TEST_ASSERT_EQUAL_UINT32(1, bootRead(c.fs, r.get()));
      TEST_ASSERT_FALSE(r->readFailed());
      TEST_ASSERT_TRUE(r->hasRoot);
      TEST_ASSERT_FALSE(r->present);
      TEST_ASSERT_TRUE_MESSAGE(r->why == cc::Why::FatRecords, cc::whyName(r->why));
      char line[160];
      cardroot::describe(*r, line, sizeof(line));
      TEST_ASSERT_EQUAL_STRING("no transfer data (the root's T tags-0000002a.bin, generation 42: fatRecords)", line);
    }
  }
  // /.mstream's own, off the volume: the way to the files is gone. (A start of 0 is
  // the root folder's to FatFs, and a folder's FR_NO_FILE is its word already.)
  Card c;
  TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir(fatmodel::path(0, ".mstream").c_str()));
  c.put(kBin, manifest());
  c.put(kTags, tags());
  c.damageEntry(nullptr, "~1      MST", 0x0FFFFFF0u);  // (FatFs's short name for it)
  std::unique_ptr<Root> r(new Root());
  TEST_ASSERT_EQUAL_UINT32(1, bootRead(c.fs, r.get()));
  TEST_ASSERT_FALSE(r->readFailed());
  TEST_ASSERT_TRUE_MESSAGE(r->why == cc::Why::Missing, cc::whyName(r->why));
}

// A glitch on the sector of /.mstream's entries, the card answering all the while (the look keeps it):
// one read of the root asks it 4 times, and the boot's waits are for this. None of it is the wrapper's to
// stop: the try after the glitch reads it.
int g_mendAtWait = 0;
void mendAtWait(uint32_t) {
  if (--g_mendAtWait == 0) fatmodel::drive(0).noAnswerLba = UINT32_MAX;
}
void test_on_fatfs_a_glitch_on_a_folders_sector_is_read_again_at_the_boot() {
  Card c;
  TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir(fatmodel::path(0, ".mstream").c_str()));
  c.put(kBin, manifest());
  c.put(kTags, tags());
  static SectorCache cache;
  TEST_ASSERT_TRUE(cache.capacity() || cache.begin(SectorCache::kDefaultEntries));
  static uint8_t scratch[fatmodel::kSS];
  fatmodel::Slot slot(0);
  fatmodel::Link link(0);
  CachedDrive wrapper(cache, slot);
  wrapper.setScratch(scratch);
  wrapper.setLink(&link);
  fatmodel::Drive& d = fatmodel::drive(0);
  d.wrapper = &wrapper;
  d.fs = &Card::fatfs;
  DIR dir;
  TEST_ASSERT_EQUAL_INT(FR_OK, f_opendir(&dir, fatmodel::path(0, ".mstream").c_str()));
  const uint32_t folder =
      static_cast<uint32_t>(Card::fatfs.database) + (static_cast<uint32_t>(dir.obj.sclust) - 2) * Card::fatfs.csize;
  f_closedir(&dir);
  c.remount();
  TEST_ASSERT_TRUE(wrapper.remember(d.disk->sectors()));
  std::unique_ptr<Root> r(new Root());
  std::vector<uint8_t> work(1024);
  d.noAnswerLba = folder;
  g_mendAtWait = 2;
  TEST_ASSERT_EQUAL_UINT32(3, cardroot::readAtBoot(c.fs, work.data(), static_cast<uint32_t>(work.size()), r.get(), mendAtWait));
  TEST_ASSERT_TRUE(r->present);
  TEST_ASSERT_FALSE(r->readFailed());
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2 * CachedDrive::kStopAfter, wrapper.kept());
  TEST_ASSERT_EQUAL_UINT32(0, wrapper.stats().stoppedReads);
}

// Which FIL holds an error, on the firmware's own FatFile: Auto DJ's new FIL turns on it.
void test_on_fatfs_which_fil_keeps_an_error() {
  Card c;
  std::vector<uint8_t> bytes(40000, 0x5A);
  c.put("/a.bin", bytes);
  c.remount();
  auto* f = static_cast<cardfat::FatFile*>(c.fs.open("/a.bin", tagstore::Fs::Mode::Read));
  TEST_ASSERT_NOT_NULL(f);
  uint8_t buf[512];
  TEST_ASSERT_TRUE(f->read(0, buf, sizeof(buf)));
  TEST_ASSERT_FALSE(f->errorKept());
  TEST_ASSERT_FALSE(f->takeReadFailed());
  // A status the card failed: the read refused, nothing kept: the remount's to reopen.
  fatmodel::Drive& d = fatmodel::drive(0);
  d.noInit = true;
  TEST_ASSERT_FALSE(f->read(16384, buf, sizeof(buf)));
  TEST_ASSERT_FALSE(f->errorKept());
  TEST_ASSERT_TRUE(f->takeReadFailed());
  TEST_ASSERT_FALSE(f->takeReadFailed());
  d.noInit = false;
  TEST_ASSERT_TRUE(f->read(16384, buf, sizeof(buf)));
  TEST_ASSERT_FALSE(f->takeReadFailed());
  // A read that failed: kept, for good, whatever comes back.
  d.failFrom = d.asked.reads;
  d.failTo = UINT64_MAX - 1;
  TEST_ASSERT_FALSE(f->read(32768, buf, sizeof(buf)));
  d.failFrom = d.failTo = UINT64_MAX;
  TEST_ASSERT_TRUE(f->errorKept());
  TEST_ASSERT_TRUE(f->takeReadFailed());
  TEST_ASSERT_FALSE(f->read(32768, buf, sizeof(buf)));
  TEST_ASSERT_TRUE(f->errorKept());
  c.fs.close(f);
}

void test_on_fatfs_each_sector_read_failed_in_its_turn_is_never_no_root() {
  for (const std::vector<uint8_t>* root : {&manifest(), &longRoot()}) {
  Card c;
  TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir(fatmodel::path(0, ".mstream").c_str()));
  c.put(kBin, *root);
  c.put(kTags, tags());
  fatmodel::Drive& d = fatmodel::drive(0);
  // A clean read's count of disk_read()s, from a fresh mount.
  c.remount();
  uint64_t asked0 = d.asked.reads;
  TEST_ASSERT_TRUE(readRoot(c.fs)->present);
  const uint64_t reads = d.asked.reads - asked0;
  TEST_ASSERT_GREATER_THAN_UINT32(3, static_cast<uint32_t>(reads));
  int unread = 0;
  for (uint64_t k = 0; k < reads; ++k) {
    c.remount();
    asked0 = d.asked.reads;
    d.failFrom = asked0 + k;
    d.failTo = d.failFrom + 1;
    auto r = readRoot(c.fs);
    d.failFrom = d.failTo = UINT64_MAX;
    char msg[80];
    std::snprintf(msg, sizeof(msg), "disk read %lu of %lu: %s", (unsigned long)k, (unsigned long)reads,
                  cc::whyName(r->why));
    TEST_ASSERT_TRUE_MESSAGE(r->present || r->readFailed(), msg);
    if (!r->present) ++unread;
    // A read later, the card answering: whole.
    TEST_ASSERT_TRUE_MESSAGE(readRoot(c.fs)->present, msg);
  }
  TEST_ASSERT_GREATER_THAN_INT32(0, unread);
  // Every read from one on: unread, whichever it is.
  for (uint64_t k = 0; k < reads; ++k) {
    c.remount();
    d.failFrom = d.asked.reads + k;
    d.failTo = UINT64_MAX - 1;
    auto r = readRoot(c.fs);
    d.failFrom = d.failTo = UINT64_MAX;
    TEST_ASSERT_TRUE(r->readFailed());
  }
  }
}

// ---- the boot's other reads, on the real FatFs ----

namespace {

namespace ts = tagstore;
constexpr const char* kD = "/.player/tags.bin";
constexpr const char* kIdx = "/.player/library.idx";
constexpr uint32_t kWhen = 0x5D4773D5;

ts::TagStore::Config storeConfig() {
  ts::TagStore::Config c;
  c.producer = "mstream-player test";
  c.parserVersion = 3;
  return c;
}

void* heapAlloc(size_t n) { return std::malloc(n ? n : 1); }
void heapFree(void* p) { std::free(p); }

bool walkInto(ts::TagStore& st, const char* rel) {
  std::vector<uint8_t> buf(4096);
  ts::WalkWriter w;
  if (!st.beginWalk(ts::Identity(), buf.data(), 4096, &w)) return false;
  ts::FolderFacts f;
  f.digest = 7;
  f.audio = 1;
  const std::string path(rel);
  const std::string folder = path.substr(0, path.find('/'));
  bool ok = w.file(rel, path.size(), 1000, kWhen, 1, LibraryBuilder::Status::Pending);
  ok = ok && w.folder(folder.data(), folder.size(), f);
  ok = ok && w.endRun(0) && w.endRun(0);
  return w.finish() && ok;
}

// The device's files, by their real writers: D, library.idx from a build of it, then a
// chunk and a walk for it to read: every file an open and a boot read. A big enough D
// and index to reach past a sector or two.
void putRecords(Card& c) {
  TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir(fatmodel::path(0, ".player").c_str()));
  TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir(fatmodel::path(0, "music").c_str()));
  {
    ts::TagStore st(c.fs, storeConfig());
    st.open();
    std::vector<uint8_t> buf(4096);
    ts::WalkWriter w;
    TEST_ASSERT_TRUE(st.beginWalk(ts::Identity(), buf.data(), 4096, &w));
    ts::FolderFacts f;
    f.digest = 7;
    for (int folder = 0; folder < 12; ++folder) {
      char dir[32];
      std::snprintf(dir, sizeof(dir), "Artist %02d", folder);
      f.audio = 8;
      for (int k = 0; k < 8; ++k) {
        char rel[64];
        std::snprintf(rel, sizeof(rel), "%s/%02d - a track with a fair name.mp3", dir, k);
        TEST_ASSERT_TRUE(w.file(rel, std::strlen(rel), 1000u + k, kWhen, 1u + k, LibraryBuilder::Status::Pending));
      }
      TEST_ASSERT_TRUE(w.folder(dir, std::strlen(dir), f));
    }
    TEST_ASSERT_TRUE(w.endRun(0));
    TEST_ASSERT_TRUE(w.endRun(0));
    TEST_ASSERT_TRUE(w.finish());
    const ts::TagStore::Compacted r = st.compact();
    TEST_ASSERT_TRUE_MESSAGE(r.ok, r.error);
    std::unique_ptr<Root> root(new Root());
    LibraryIndex index(heapAlloc, heapFree);
    LibraryUpdate::Config u;
    u.fs = &c.fs;
    u.store = &st;
    u.root = root.get();
    u.index = &index;
    LibraryUpdate update(u);
    const LibraryUpdate::Booted b = update.boot();
    TEST_ASSERT_TRUE(b.built);
    TEST_ASSERT_FALSE(b.saveFailed);
    uint8_t chunkBuf[2048];
    ts::ChunkBuilder chunk;
    chunk.begin(chunkBuf, sizeof(chunkBuf));
    cc::mptg::Record rec;
    rec.size = 1000;
    rec.fatTime = kWhen;
    const char* fields[cc::kRunFields] = {};
    for (auto& x : fields) x = "";
    TEST_ASSERT_TRUE(chunk.add("Artist 00/00 - a track with a fair name.mp3", 43, LibraryBuilder::Status::Scanned, rec, fields));
    TEST_ASSERT_TRUE(st.append(chunk));
    TEST_ASSERT_TRUE(walkInto(st, "Artist 99/new.mp3"));
  }
  c.remount();
}

std::vector<uint8_t> bytesOf(Card& c, const char* path) {
  tagstore::File* f = c.fs.open(path, tagstore::Fs::Mode::Read);
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::vector<uint8_t> b(f->size());
  if (!b.empty()) TEST_ASSERT_TRUE(f->read(0, b.data(), f->size()));
  c.fs.close(f);
  return b;
}

// Reading the disk from `first` on, for `count` reads, what `read` does; from a mount
// made anew, so it reads the card from the boot sector up.
template <class Read>
void withDiskFault(Card& c, uint64_t first, uint64_t count, Read read) {
  fatmodel::Drive& d = fatmodel::drive(0);
  c.remount();
  d.failFrom = d.asked.reads + first;
  d.failTo = count == UINT64_MAX ? UINT64_MAX - 1 : d.failFrom + count;
  read();
  d.failFrom = d.failTo = UINT64_MAX;
}

}  // namespace

// D, tags.jnl and walk.jnl, each sector read failed in its turn, and every read from
// there on: the records are read whole, or the open says the card failed. Never no D.
void test_on_fatfs_the_records_are_never_gone_for_a_read_that_failed() {
  Card c;
  putRecords(c);
  const std::vector<uint8_t> d0 = bytesOf(c, kD), j0 = bytesOf(c, "/.player/tags.jnl"), w0 = bytesOf(c, "/.player/walk.jnl");
  fatmodel::Drive& d = fatmodel::drive(0);
  c.remount();
  const uint64_t asked0 = d.asked.reads;
  {
    ts::TagStore st(c.fs, storeConfig());
    const ts::TagStore::Opened o = st.open();
    TEST_ASSERT_FALSE(o.readFailed);
    TEST_ASSERT_TRUE(st.device().present);
    TEST_ASSERT_EQUAL_UINT32(1, st.chunkCount());
    TEST_ASSERT_TRUE(st.hasWalk());
  }
  const uint64_t reads = d.asked.reads - asked0;
  TEST_ASSERT_GREATER_THAN_UINT32(6, static_cast<uint32_t>(reads));
  uint32_t unread = 0;
  for (uint64_t k = 0; k < reads; ++k) {
    char msg[64];
    std::snprintf(msg, sizeof(msg), "disk read %lu of %lu", (unsigned long)k, (unsigned long)reads);
    for (const uint64_t count : {uint64_t{1}, uint64_t{UINT64_MAX}}) {
      ts::TagStore st(c.fs, storeConfig());
      ts::TagStore::Opened o;
      withDiskFault(c, k, count, [&] { o = st.open(); });
      TEST_ASSERT_TRUE_MESSAGE(o.readFailed, msg);
      TEST_ASSERT_TRUE_MESSAGE(st.readFailed(), msg);
      TEST_ASSERT_FALSE_MESSAGE(st.device().present, msg);
      TEST_ASSERT_FALSE_MESSAGE(st.compact().ok, msg);
      ++unread;
      // A read later, the card answering: all there, and nothing touched.
      const ts::TagStore::Opened again = st.open();
      TEST_ASSERT_FALSE_MESSAGE(again.readFailed, msg);
      TEST_ASSERT_TRUE_MESSAGE(st.device().present, msg);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, st.chunkCount(), msg);
      TEST_ASSERT_TRUE_MESSAGE(st.hasWalk(), msg);
    }
  }
  TEST_ASSERT_GREATER_THAN_UINT32(0, unread);
  TEST_ASSERT_TRUE(d0 == bytesOf(c, kD));
  TEST_ASSERT_TRUE(j0 == bytesOf(c, "/.player/tags.jnl"));
  TEST_ASSERT_TRUE(w0 == bytesOf(c, "/.player/walk.jnl"));
}

// library.idx, the same: the boot's every sector failed in its turn. It says unread, or
// reads it whole; never missing or corrupt. It writes nothing, and holds the jobs.
void test_on_fatfs_library_idx_is_never_missing_or_corrupt_for_a_read_that_failed() {
  Card c;
  putRecords(c);
  std::unique_ptr<Root> root(new Root());
  // (Merged and built again, so a boot's own reads are library.idx's, its marker's and
  // its settling's, and a build for the session gives the library it holds.)
  {
    ts::TagStore st(c.fs, storeConfig());
    st.open();
    TEST_ASSERT_TRUE(st.compact().ok);
    TEST_ASSERT_TRUE(c.fs.remove(kIdx));
    LibraryIndex index(heapAlloc, heapFree);
    LibraryUpdate::Config u;
    u.fs = &c.fs;
    u.store = &st;
    u.root = root.get();
    u.index = &index;
    LibraryUpdate update(u);
    const LibraryUpdate::Booted b = update.boot();
    TEST_ASSERT_TRUE(b.built);
    TEST_ASSERT_FALSE(b.saveFailed);
  }
  const std::vector<uint8_t> idx0 = bytesOf(c, kIdx), d0 = bytesOf(c, kD);
  const auto bootOn = [&](bool withFault, uint64_t k, uint64_t count) {
    ts::TagStore st(c.fs, storeConfig());
    st.open();
    TEST_ASSERT_TRUE(st.device().present);
    LibraryIndex index(heapAlloc, heapFree);
    LibraryUpdate::Config u;
    u.fs = &c.fs;
    u.store = &st;
    u.root = root.get();
    u.index = &index;
    LibraryUpdate update(u);
    LibraryUpdate::Booted b;
    if (withFault) {
      fatmodel::Drive& d = fatmodel::drive(0);
      d.failFrom = d.asked.reads + k;
      d.failTo = count == UINT64_MAX ? UINT64_MAX - 1 : d.failFrom + count;
    }
    b = update.boot();
    fatmodel::drive(0).failFrom = fatmodel::drive(0).failTo = UINT64_MAX;
    if (b.ok) TEST_ASSERT_EQUAL_UINT32(97, index.trackCount());  // (whole, or none: no walk here)
    return b;
  };
  fatmodel::Drive& d = fatmodel::drive(0);
  c.remount();
  // The clean boot's count of disk reads after the store's open.
  uint64_t reads = 0;
  {
    ts::TagStore st(c.fs, storeConfig());
    st.open();
    LibraryIndex index(heapAlloc, heapFree);
    LibraryUpdate::Config u;
    u.fs = &c.fs;
    u.store = &st;
    u.root = root.get();
    u.index = &index;
    LibraryUpdate update(u);
    const uint64_t asked0 = d.asked.reads;
    const LibraryUpdate::Booted b = update.boot();
    reads = d.asked.reads - asked0;
    TEST_ASSERT_TRUE(b.saved == libraryboot::Saved::Matches);
    TEST_ASSERT_TRUE(b.decision.action == libraryboot::Action::Load);
    TEST_ASSERT_TRUE(b.ok);
    TEST_ASSERT_EQUAL_UINT32(97, index.trackCount());
  }
  // (Past the clean count too: from a fresh mount a read of the directory comes first.)
  uint32_t unread = 0;
  for (uint64_t k = 0; k < reads + 4; ++k) {
    char msg[64];
    std::snprintf(msg, sizeof(msg), "disk read %lu of %lu", (unsigned long)k, (unsigned long)reads);
    for (const uint64_t count : {uint64_t{1}, uint64_t{UINT64_MAX}}) {
      c.remount();
      const LibraryUpdate::Booted b = bootOn(true, k, count);
      TEST_ASSERT_TRUE_MESSAGE(b.saved == libraryboot::Saved::Unread || b.saved == libraryboot::Saved::Matches, msg);
      if (b.saved == libraryboot::Saved::Unread) {
        ++unread;
        TEST_ASSERT_TRUE_MESSAGE(b.decision.holdJobs, msg);
        TEST_ASSERT_TRUE_MESSAGE(b.decision.action != libraryboot::Action::Load, msg);
      }
      TEST_ASSERT_TRUE_MESSAGE(idx0 == bytesOf(c, kIdx), msg);
      TEST_ASSERT_TRUE_MESSAGE(d0 == bytesOf(c, kD), msg);
    }
  }
  TEST_ASSERT_GREATER_THAN_UINT32(0, unread);
}

// FatFs reads the card, every read answered, and finds a file's records inconsistent
// (FR_INT_ERR): no read again changes that. So it is the file's fault, and a boot that
// took it for the card's would hold on to it for ever.
void test_on_fatfs_a_damaged_directory_entry_is_the_files_fault_at_the_boot_too() {
  for (const uint32_t cluster : {0u, 1u, 0x0FFFFFF0u}) {
    // D's: absent, with the journals of a D no more.
    {
      Card c;
      putRecords(c);
      c.damageEntry(".player", "TAGS    BIN", cluster);
      ts::TagStore st(c.fs, storeConfig());
      const ts::TagStore::Opened o = st.open();
      TEST_ASSERT_FALSE(o.readFailed);
      TEST_ASSERT_FALSE(st.device().present);
      TEST_ASSERT_EQUAL_STRING("fatRecords", cc::whyName(o.deviceWhy));
    }
    // A journal's: D stands, and it is dropped as a torn one is.
    for (const char* jnl : {"TAGS    JNL", "WALK    JNL"}) {
      Card c;
      putRecords(c);
      c.damageEntry(".player", jnl, cluster);
      ts::TagStore st(c.fs, storeConfig());
      const ts::TagStore::Opened o = st.open();
      TEST_ASSERT_FALSE_MESSAGE(o.readFailed, jnl);
      TEST_ASSERT_TRUE_MESSAGE(st.device().present, jnl);
    }
    // library.idx's: corrupt, and built over as one is. Nothing held.
    {
      Card c;
      putRecords(c);
      c.damageEntry(".player", "LIBRARY IDX", cluster);
      ts::TagStore st(c.fs, storeConfig());
      st.open();
      std::unique_ptr<Root> root(new Root());
      LibraryIndex index(heapAlloc, heapFree);
      LibraryUpdate::Config u;
      u.fs = &c.fs;
      u.store = &st;
      u.root = root.get();
      u.index = &index;
      LibraryUpdate update(u);
      const LibraryUpdate::Booted b = update.boot();
      TEST_ASSERT_TRUE(b.saved == libraryboot::Saved::Corrupt);
      TEST_ASSERT_FALSE(b.decision.holdJobs);
      TEST_ASSERT_TRUE(b.decision.action == libraryboot::Action::Build);
      TEST_ASSERT_TRUE(b.ok);
    }
  }
  // /.player's own, off the volume: the way to the files is gone. Its word too.
  Card c;
  putRecords(c);
  c.damageEntry(nullptr, "~1      PLA", 0x0FFFFFF0u);  // (FatFs's short name for it)
  ts::TagStore st(c.fs, storeConfig());
  const ts::TagStore::Opened o = st.open();
  TEST_ASSERT_FALSE(o.readFailed);
  TEST_ASSERT_FALSE(st.device().present);
}

// entry(): FatFs's word, and what it says of a chain.
void test_on_fatfs_an_entry_says_whose_word_it_is() {
  Card c;
  putRecords(c);
  tagstore::Fs::Entry e = c.fs.entry(kD);
  TEST_ASSERT_TRUE(e.there);
  TEST_ASSERT_FALSE(e.cardFailed);
  TEST_ASSERT_NOT_EQUAL(0, e.firstCluster);
  TEST_ASSERT_EQUAL_UINT32(c.fs.firstCluster(kD), e.firstCluster);
  e = c.fs.entry("/.player/no such file");
  TEST_ASSERT_FALSE(e.there);
  TEST_ASSERT_FALSE(e.cardFailed);
  e = c.fs.entry("/no such folder/file");
  TEST_ASSERT_FALSE(e.there);
  TEST_ASSERT_FALSE(e.cardFailed);
  bool failed = false;
  withDiskFault(c, 0, UINT64_MAX, [&] {
    e = c.fs.entry(kD);
    failed = e.cardFailed;
  });
  TEST_ASSERT_TRUE(failed);
  TEST_ASSERT_FALSE(e.there);
}


// ---- the queue's file, on the real FatFs (lib/core QueueFile) ----

namespace {

constexpr const char* kQueue = "/.player/queue.txt";
constexpr const char* kQueueTmp = "/.player/queue.tmp";
constexpr const char* kTrackA = "/music/Band/Live/01 - a.mp3";
constexpr const char* kTrackB = "/music/Band/Live/02 - b.mp3";
constexpr const char* kTrackC = "/music/Loose/03 - c.mp3";

std::vector<uint8_t> bytesOf(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// A queue's file of a track a line.
std::string queueText(const std::vector<std::string>& lines, int current = 0, int generation = 5) {
  std::string t = "mstream-queue 1 " + std::to_string(lines.size()) + " " + std::to_string(current) + " " +
                  std::to_string(generation) + "\n";
  for (const std::string& l : lines) t += l + "\n";
  return t;
}

// The library the queue reads against, the player's own files on the card under `c`.
struct QueueCard : Card {
  LibraryIndex idx;
  TrackCatalog::Unlisted table;
  TrackCatalog catalog{&idx};
  QueueModel q;
  queuefile::Lookup lookup{fs, table};
  queuefile::FsFiles files{fs};
  std::vector<uint32_t> waits;
  QueueCard() {
    catalog.setUnlisted(&table);
    TEST_ASSERT_TRUE(idx.begin("/music"));
    TEST_ASSERT_TRUE(idx.addFile(kTrackA) == LibraryIndex::Add::Added);
    TEST_ASSERT_TRUE(idx.finish());
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir(fatmodel::path(0, ".player").c_str()));
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir(fatmodel::path(0, "music").c_str()));
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir(fatmodel::path(0, "music/Band").c_str()));
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir(fatmodel::path(0, "music/Band/Live").c_str()));
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir(fatmodel::path(0, "music/Loose").c_str()));
    put(kTrackA, bytesOf("a"));
    put(kTrackB, bytesOf("b"));
  }
  queuefile::Read read(bool waiting = true, bool asking = true) {
    return queuefile::readAtBoot(files, kQueue, kQueueTmp, catalog, q, nullptr, nullptr, nullptr, nullptr,
                                 waiting ? noteWait : nullptr, asking ? &lookup : nullptr);
  }
  static std::vector<uint32_t>* s_waits;
  static void noteWait(uint32_t ms) {
    if (s_waits) s_waits->push_back(ms);
  }
};
std::vector<uint32_t>* QueueCard::s_waits = nullptr;

uint32_t g_lookupNow = 0, g_lookupStep = 0;
uint32_t lookupNow() { return g_lookupNow += g_lookupStep; }

}  // namespace

// look(): FatFs's word, on a file, a folder, and what isn't there.
void test_on_fatfs_look_says_whose_word_it_is() {
  QueueCard c;
  using Seen = tagstore::Fs::Seen;
  TEST_ASSERT_TRUE(c.fs.look(kTrackA) == Seen::There);
  TEST_ASSERT_TRUE(c.fs.look("/music/Band/Live/09 - no.mp3") == Seen::Gone);  // FR_NO_FILE
  TEST_ASSERT_TRUE(c.fs.look("/music/No Band/Live/09 - no.mp3") == Seen::Gone);  // FR_NO_PATH
  TEST_ASSERT_TRUE(c.fs.look("/music/Band/Live") == Seen::Gone);  // a folder: no such file
  TEST_ASSERT_TRUE(c.fs.look("/music/Band/Li*ve.mp3") == Seen::Gone);  // a name FatFs can't write
  // The card fails: it says nothing, wherever the read falls on the way.
  c.remount();
  fatmodel::Drive& d = fatmodel::drive(0);
  uint64_t asked0 = d.asked.reads;
  TEST_ASSERT_TRUE(c.fs.look(kTrackA) == Seen::There);
  const uint64_t reads = d.asked.reads - asked0;
  TEST_ASSERT_GREATER_THAN_UINT32(2, static_cast<uint32_t>(reads));
  for (uint64_t k = 0; k < reads; ++k) {
    c.remount();
    d.failFrom = d.asked.reads + k;
    d.failTo = d.failFrom + 1;
    const Seen s = c.fs.look(kTrackA);
    d.failFrom = d.failTo = UINT64_MAX;
    TEST_ASSERT_TRUE(s == Seen::Unknown);
    TEST_ASSERT_TRUE(c.fs.look(kTrackA) == Seen::There);
  }
  // The file system's records of the way to it are damaged (FR_INT_ERR): no fault of the
  // card's, and no word that it is gone: a disk check may bring it back.
  c.damageEntry(nullptr, "MUSIC", 0x0FFFFFF0u);
  TEST_ASSERT_TRUE(c.fs.look(kTrackA) == Seen::Unknown);
}

// Case 1 of 3.2.2: a read of queue.txt the card fails, in its turn, is never no queue.
void test_on_fatfs_a_read_of_queue_txt_that_fails_is_never_no_queue() {
  for (const size_t lines : {size_t{2}, size_t{40}}) {
    QueueCard c;
    std::vector<std::string> want;
    for (size_t i = 0; i < lines; ++i) want.push_back(kTrackA);
    c.put(kQueue, bytesOf(queueText(want, 1)));
    fatmodel::Drive& d = fatmodel::drive(0);
    c.remount();
    uint64_t asked0 = d.asked.reads;
    queuefile::Read r = c.read();
    TEST_ASSERT_TRUE(r.got == queuefile::Got::Whole);
    TEST_ASSERT_EQUAL_UINT32(1, r.reads);
    TEST_ASSERT_EQUAL_size_t(lines, c.q.size());
    const uint64_t reads = d.asked.reads - asked0;
    TEST_ASSERT_GREATER_THAN_UINT32(2, static_cast<uint32_t>(reads));
    for (uint64_t k = 0; k < reads; ++k) {
      char msg[80];
      std::snprintf(msg, sizeof(msg), "disk read %lu of %lu, %lu lines", (unsigned long)k, (unsigned long)reads,
                    (unsigned long)lines);
      // Once: a glitch. The next read has it, after one wait.
      c.remount();
      c.q.release();
      std::vector<uint32_t> waits;
      QueueCard::s_waits = &waits;
      d.failFrom = d.asked.reads + k;
      d.failTo = d.failFrom + 1;
      r = c.read();
      d.failFrom = d.failTo = UINT64_MAX;
      QueueCard::s_waits = nullptr;
      TEST_ASSERT_TRUE_MESSAGE(r.got == queuefile::Got::Whole, msg);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(lines, c.q.size(), msg);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(kQueue, r.path, msg);
      TEST_ASSERT_EQUAL_INT_MESSAGE(1, c.q.current(), msg);
      TEST_ASSERT_LESS_OR_EQUAL_UINT32_MESSAGE(2, r.reads, msg);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(r.reads - 1, waits.size(), msg);
      // Every read from there on: unread, the queue left alone, the waits spent in order.
      c.remount();
      c.q.release();
      waits.clear();
      QueueCard::s_waits = &waits;
      d.failFrom = d.asked.reads + k;
      d.failTo = UINT64_MAX - 1;
      r = c.read();
      d.failFrom = d.failTo = UINT64_MAX;
      QueueCard::s_waits = nullptr;
      TEST_ASSERT_TRUE_MESSAGE(r.got == queuefile::Got::Unread, msg);
      TEST_ASSERT_TRUE_MESSAGE(r.faulted, msg);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(0, c.q.size(), msg);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(4, r.reads, msg);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(3, waits.size(), msg);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(tagstore::kBootWaitsMs[0], waits[0], msg);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(tagstore::kBootWaitsMs[2], waits[2], msg);
      // No waits to spend (an earlier reader spent them): one read.
      c.remount();
      d.failFrom = d.asked.reads + k;
      d.failTo = UINT64_MAX - 1;
      r = c.read(false);
      d.failFrom = d.failTo = UINT64_MAX;
      TEST_ASSERT_TRUE_MESSAGE(r.got == queuefile::Got::Unread, msg);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, r.reads, msg);
      // The card answers again: all of it, in its place.
      c.remount();
      r = c.read();
      TEST_ASSERT_TRUE_MESSAGE(r.got == queuefile::Got::Whole, msg);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(lines, c.q.size(), msg);
    }
  }
}

// The read that fails in the last line, on a sector that holds only its tail: the line
// before it reads as that line, and the count holds. Still no queue, and no line lost.
void test_on_fatfs_a_read_that_fails_in_the_last_line_is_no_whole_file() {
  QueueCard c;
  const std::string track(kTrackA);
  std::vector<std::string> lines;
  std::string text = queueText({});
  // Lines until the file ends a few bytes into a sector.
  for (size_t n = 0; n < 200; ++n) {
    lines.push_back(kTrackA);
    text = queueText(lines);
    if (text.size() % 512 > 0 && text.size() % 512 < track.size()) break;
  }
  TEST_ASSERT_LESS_THAN(track.size(), text.size() % 512);
  c.put(kQueue, bytesOf(text));
  tagstore::File* f = c.fs.open(kQueue, tagstore::Fs::Mode::Read);
  TEST_ASSERT_NOT_NULL(f);
  uint8_t b[8];
  TEST_ASSERT_TRUE(f->read(text.size() - 4, b, sizeof(b) - 4));
  const uint32_t tail = static_cast<uint32_t>(static_cast<cardfat::FatFile*>(f)->fil.sect);
  c.fs.close(f);
  fatmodel::Drive& d = fatmodel::drive(0);
  // For good: unread, four reads, the queue left alone.
  c.remount();
  d.failSector = tail;
  queuefile::Read r = c.read();
  d.failSector = UINT64_MAX;
  TEST_ASSERT_TRUE(r.got == queuefile::Got::Unread);
  TEST_ASSERT_EQUAL_UINT32(4, r.reads);
  TEST_ASSERT_TRUE(r.faulted);
  TEST_ASSERT_EQUAL_size_t(0, c.q.size());
  // Mended: every line.
  c.remount();
  r = c.read();
  TEST_ASSERT_TRUE(r.got == queuefile::Got::Whole);
  TEST_ASSERT_FALSE(r.faulted);
  TEST_ASSERT_EQUAL_UINT32(lines.size(), r.read.entries);
  TEST_ASSERT_EQUAL_UINT32(0, r.read.dropped);
}

// A read again, to leave out a line the card says is gone, fails with the file already
// whole: the first read stands, and the caller hears of the fault (a read with no answer
// makes FatFs's next call a remount: main.cpp's guard restarts at one under a card job).
void test_on_fatfs_a_read_again_that_fails_says_so() {
  QueueCard c;
  c.put(kQueue, bytesOf(queueText({kTrackA, kTrackB, "/music/Band/Live/09 - gone.mp3"})));
  fatmodel::Drive& d = fatmodel::drive(0);
  c.remount();
  const uint64_t asked0 = d.asked.reads;
  queuefile::Read r = c.read();
  TEST_ASSERT_TRUE(r.got == queuefile::Got::Whole);
  TEST_ASSERT_FALSE(r.faulted);
  TEST_ASSERT_EQUAL_UINT32(1, c.lookup.gone());
  TEST_ASSERT_EQUAL_UINT32(2, r.read.entries);
  const uint64_t reads = d.asked.reads - asked0;
  // The last disk read is the read again's.
  c.remount();
  c.q.release();
  d.failFrom = d.asked.reads + reads - 1;
  d.failTo = d.failFrom + 1;
  r = c.read();
  d.failFrom = d.failTo = UINT64_MAX;
  TEST_ASSERT_TRUE(r.got == queuefile::Got::Whole);
  TEST_ASSERT_TRUE(r.faulted);
  TEST_ASSERT_EQUAL_UINT32(1, r.reads);
  TEST_ASSERT_EQUAL_UINT32(3, r.read.entries);  // the first read's, the gone line named nothing
  TEST_ASSERT_EQUAL_size_t(3, c.q.size());
  char path[TrackCatalog::kMaxPath];
  c.catalog.path(c.q.trackAt(1), path, sizeof(path));
  TEST_ASSERT_EQUAL_STRING(kTrackB, path);
  TEST_ASSERT_EQUAL_UINT32(0, c.catalog.path(c.q.trackAt(2), path, sizeof(path)));
}

namespace {

// The read's memory (the firmware's PSRAM) with a block of one size refused: `refusals`
// more times, or every time (-1).
struct Short {
  static size_t refuse;
  static int refusals;
  static void* alloc(size_t n) {
    if (n == refuse && refusals != 0) {
      if (refusals > 0) --refusals;
      return nullptr;
    }
    return std::malloc(n ? n : 1);
  }
  static void release(void* p) { std::free(p); }
};
size_t Short::refuse = 0;
int Short::refusals = 0;

}  // namespace

// No memory for the read's blocks, or for the queue's own (Restored::noMemory), is no word
// on the file either: Unread, read again after the boot's waits, the queue left alone. Taken
// for a file that isn't whole, the boot's default queue went over it (3.2.2). Memory back by
// a later read: the file, at its place. A whole queue.tmp stands in, as for a read the card
// fails.
void test_on_fatfs_no_memory_to_read_queue_txt_is_unread_not_no_queue() {
  QueueCard c;
  c.put(kQueue, bytesOf(queueText({kTrackA, kTrackA, kTrackA}, 1)));
  const auto read = [&](QueueModel& q, std::vector<uint32_t>* waits) {
    QueueCard::s_waits = waits;
    const queuefile::Read r = queuefile::readAtBoot(c.files, kQueue, kQueueTmp, c.catalog, q, nullptr, nullptr,
                                                    Short::alloc, Short::release, QueueCard::noteWait, &c.lookup);
    QueueCard::s_waits = nullptr;
    return r;
  };
  // The read's ids (3 lines, 12 B), then the queue's own block (12 B an entry).
  for (const size_t block : {size_t{3 * 4}, size_t{3 * 12}}) {
    char msg[32];
    std::snprintf(msg, sizeof(msg), "a block of %u B", static_cast<unsigned>(block));
    QueueModel q(Short::alloc, Short::release);
    const uint32_t old[] = {0, 0};
    TEST_ASSERT_TRUE(q.assign(old, 2, 0));
    Short::refuse = block;
    Short::refusals = -1;
    std::vector<uint32_t> waits;
    queuefile::Read r = read(q, &waits);
    TEST_ASSERT_TRUE_MESSAGE(r.got == queuefile::Got::Unread, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.noMemory, msg);
    TEST_ASSERT_FALSE_MESSAGE(r.faulted, msg);  // (no card read failed: nothing for FatFs to mount again)
    TEST_ASSERT_NULL_MESSAGE(r.notWhole, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(4, r.reads, msg);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(3, waits.size(), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, q.size(), msg);  // left alone
    // Refused once: the read after the first wait has it, at its line.
    Short::refusals = 1;
    waits.clear();
    r = read(q, &waits);
    TEST_ASSERT_TRUE_MESSAGE(r.got == queuefile::Got::Whole, msg);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(kQueue, r.path, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, r.reads, msg);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(1, waits.size(), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, q.size(), msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, q.current(), msg);
  }
  // A whole queue.tmp beside it (2 lines: blocks of another size) stands in, read once.
  c.put(kQueueTmp, bytesOf(queueText({kTrackB, kTrackA}, 0)));
  {
    QueueModel q(Short::alloc, Short::release);
    Short::refuse = 3 * 4;
    Short::refusals = -1;
    const queuefile::Read r = read(q, nullptr);
    TEST_ASSERT_TRUE(r.got == queuefile::Got::Whole);
    TEST_ASSERT_EQUAL_STRING(kQueueTmp, r.path);
    TEST_ASSERT_TRUE(r.noMemory);
    TEST_ASSERT_EQUAL_UINT32(1, r.reads);
    TEST_ASSERT_EQUAL_UINT32(2, q.size());
  }
  // No memory for it either: unread, not queue.tmp's word.
  c.put(kQueueTmp, bytesOf(queueText({kTrackB, kTrackA, kTrackA}, 0)));
  {
    QueueModel q(Short::alloc, Short::release);
    const queuefile::Read r = read(q, nullptr);
    TEST_ASSERT_TRUE(r.got == queuefile::Got::Unread);
    TEST_ASSERT_EQUAL_UINT32(4, r.reads);
  }
  Short::refuse = 0;
  Short::refusals = 0;
}

// What is the file's own fault, or no file at all: None, read once, so the caller's
// default queue goes on as it always has.
void test_on_fatfs_no_queue_txt_a_cut_or_damaged_one_are_no_queue() {
  {
    QueueCard c;  // (no file: FR_NO_FILE, no waits)
    std::vector<uint32_t> waits;
    QueueCard::s_waits = &waits;
    queuefile::Read r = c.read();
    QueueCard::s_waits = nullptr;
    TEST_ASSERT_TRUE(r.got == queuefile::Got::None);
    TEST_ASSERT_EQUAL_UINT32(1, r.reads);
    TEST_ASSERT_EQUAL_size_t(0, waits.size());
    TEST_ASSERT_NULL(r.notWhole);
    // Cut short, a line less than it says: every read answered. (Nothing asked of the
    // card about its lines: it isn't a queue.)
    std::string t = queueText({kTrackB, kTrackB, kTrackB});
    t.resize(t.size() - std::strlen(kTrackB) - 1);
    c.put(kQueue, bytesOf(t));
    r = c.read();
    TEST_ASSERT_TRUE(r.got == queuefile::Got::None);
    TEST_ASSERT_EQUAL_UINT32(1, r.reads);
    TEST_ASSERT_EQUAL_STRING(kQueue, r.notWhole);
    TEST_ASSERT_EQUAL_UINT32(0, c.lookup.kept() + c.lookup.gone() + c.lookup.unasked());
  }
  // Its own directory entry damaged (FR_INT_ERR, at the open or the first read): no read
  // again changes that.
  for (const uint32_t cluster : {1u, 0x0FFFFFF0u}) {
    QueueCard c;
    c.put(kQueue, bytesOf(queueText({kTrackA, kTrackA})));
    c.damageEntry(".player", "QUEUE", cluster);
    const queuefile::Read r = c.read();
    TEST_ASSERT_TRUE(r.got == queuefile::Got::None);
    TEST_ASSERT_EQUAL_UINT32(1, r.reads);
  }
}

// queue.tmp is only whole when a cut fell after its flush: the newest. It stands in for a
// queue.txt that is gone, as ever, and for one nobody could read; one that isn't whole, for neither.
void test_on_fatfs_queue_tmp_stands_in() {
  {
    QueueCard c;
    c.put(kQueueTmp, bytesOf(queueText({kTrackA})));
    queuefile::Read r = c.read();
    TEST_ASSERT_TRUE(r.got == queuefile::Got::Whole);
    TEST_ASSERT_EQUAL_STRING(kQueueTmp, r.path);
    // Beside a queue.txt: queue.txt's, as ever.
    c.put(kQueue, bytesOf(queueText({kTrackA, kTrackA})));
    r = c.read();
    TEST_ASSERT_EQUAL_STRING(kQueue, r.path);
    TEST_ASSERT_EQUAL_size_t(2, c.q.size());
    // queue.txt's data sector fails, for good: queue.tmp.
    c.remount();
    tagstore::File* f = c.fs.open(kQueue, tagstore::Fs::Mode::Read);
    TEST_ASSERT_NOT_NULL(f);
    uint8_t b[8];
    TEST_ASSERT_TRUE(f->read(0, b, sizeof(b)));
    const uint32_t sector = static_cast<uint32_t>(static_cast<cardfat::FatFile*>(f)->fil.sect);
    c.fs.close(f);
    fatmodel::Drive& d = fatmodel::drive(0);
    c.remount();
    d.failSector = sector;
    r = c.read();
    d.failSector = UINT64_MAX;
    TEST_ASSERT_TRUE(r.got == queuefile::Got::Whole);
    TEST_ASSERT_EQUAL_STRING(kQueueTmp, r.path);
    TEST_ASSERT_EQUAL_size_t(1, c.q.size());
    // And with queue.tmp not whole: unread, not no queue.
    c.put(kQueueTmp, bytesOf(std::string("not a queue")));
    c.remount();
    d.failSector = sector;
    r = c.read();
    d.failSector = UINT64_MAX;
    TEST_ASSERT_TRUE(r.got == queuefile::Got::Unread);
  }
}

// Case 2 of 3.2.2: a line the library doesn't list leaves only when the card says it's gone.
void test_on_fatfs_a_line_leaves_only_when_its_file_is_gone() {
  QueueCard c;
  const std::string gone = "/music/Band/Live/09 - gone.mp3";
  const std::string noFolder = "/music/No Band/Album/01 - gone.mp3";
  c.put(kQueue, bytesOf(queueText({kTrackA, kTrackB, gone, noFolder, "tone:no such tone", kTrackB}, 2)));
  queuefile::Read r = c.read();
  TEST_ASSERT_TRUE(r.got == queuefile::Got::Whole);
  TEST_ASSERT_EQUAL_UINT32(3, r.read.entries);
  TEST_ASSERT_EQUAL_UINT32(3, r.read.dropped);
  TEST_ASSERT_EQUAL_UINT32(0, r.read.held);
  TEST_ASSERT_EQUAL_UINT32(2, c.lookup.kept());
  TEST_ASSERT_EQUAL_UINT32(2, c.lookup.gone());
  TEST_ASSERT_EQUAL_UINT32(0, c.lookup.unasked());
  TEST_ASSERT_FALSE(c.lookup.failed());
  char path[TrackCatalog::kMaxPath];
  c.catalog.path(c.q.trackAt(1), path, sizeof(path));
  TEST_ASSERT_EQUAL_STRING(kTrackB, path);
  c.catalog.path(c.q.trackAt(2), path, sizeof(path));
  TEST_ASSERT_EQUAL_STRING(kTrackB, path);
  // The current line was the first gone: the next that stayed.
  TEST_ASSERT_EQUAL_INT(2, c.q.current());
  TEST_ASSERT_FALSE(r.read.currentKept);
  // A library that lists it: no question.
  c.put(kQueue, bytesOf(queueText({kTrackA})));
  r = c.read();
  TEST_ASSERT_EQUAL_UINT32(1, r.reads);
  TEST_ASSERT_EQUAL_UINT32(0, c.lookup.kept() + c.lookup.gone() + c.lookup.unasked());
  // No lookup given (no library to read against): dropped, no question.
  c.put(kQueue, bytesOf(queueText({kTrackB, kTrackA})));
  r = c.read(true, false);
  TEST_ASSERT_EQUAL_UINT32(1, r.read.entries);
  TEST_ASSERT_EQUAL_UINT32(1, r.read.dropped);
}

// A question the card fails, wherever it falls: that line stays, and every line after it
// with no more questions (a card that has stopped answering costs about 0.6 s a question).
void test_on_fatfs_a_question_that_fails_keeps_the_lines_and_stops_asking() {
  QueueCard c;
  c.put(kTrackC, bytesOf("c"));
  const std::vector<std::string> lines = {kTrackB, "/music/Band/Live/09 - gone.mp3", kTrackB, kTrackC};
  fatmodel::Drive& d = fatmodel::drive(0);
  // The queue's file read first, on its own, so the fault below falls on a question.
  const std::string text = queueText(lines);
  c.put(kQueue, bytesOf(text));
  c.remount();
  const auto asked = [&](queuefile::Lookup& lookup) {
    lookup.begin();
    MemorySource first(text.data(), text.size());
    queuetext::Restored r = queuetext::read(first, c.catalog, c.q, nullptr, nullptr, nullptr, nullptr, &lookup);
    if (r.ok && lookup.settle()) {
      MemorySource again(text.data(), text.size());
      r = queuetext::read(again, c.catalog, c.q, nullptr, nullptr, nullptr, nullptr, &lookup);
    }
    return r;
  };
  // Read through memory: only the questions go to the card.
  for (uint64_t k = 0; k < 8; ++k) {
    c.q.release();
    c.remount();
    d.failFrom = d.asked.reads + k;
    d.failTo = d.failFrom + 1;
    const queuetext::Restored r = asked(c.lookup);
    d.failFrom = d.failTo = UINT64_MAX;
    TEST_ASSERT_TRUE(r.ok);
    // No line that may be on the card left out, and only the card's own word drops one.
    TEST_ASSERT_EQUAL_UINT32(4 - c.lookup.gone(), r.entries);
    TEST_ASSERT_EQUAL_UINT32(c.lookup.kept() + c.lookup.unasked(), r.entries);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1, c.lookup.gone());
    if (c.lookup.failed()) TEST_ASSERT_GREATER_THAN_UINT32(0, c.lookup.unasked());
  }
  // Every read failing: nothing asked past the first, all four by their paths.
  c.q.release();
  c.remount();
  d.failFrom = d.asked.reads;
  d.failTo = UINT64_MAX - 1;
  const uint64_t asked0 = d.asked.reads;
  const queuetext::Restored r = asked(c.lookup);
  const uint64_t reads = d.asked.reads - asked0;
  d.failFrom = d.failTo = UINT64_MAX;
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(4, r.entries);
  TEST_ASSERT_EQUAL_UINT32(4, c.lookup.unasked());
  TEST_ASSERT_TRUE(c.lookup.failed());
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(2, static_cast<uint32_t>(reads));
}

// The time is up: the lines left stay by their paths, for the next read to ask about.
void test_on_fatfs_a_read_asks_no_longer_than_its_time() {
  QueueCard c;
  const std::vector<std::string> lines = {"/music/Band/Live/09 - gone.mp3", "/music/Band/Live/08 - gone.mp3"};
  const std::string text = queueText(lines);
  c.put(kQueue, bytesOf(text));
  c.lookup.limit(lookupNow, 100);
  g_lookupStep = 0;
  queuefile::Read r = c.read();
  TEST_ASSERT_EQUAL_UINT32(2, c.lookup.gone());
  TEST_ASSERT_EQUAL_UINT32(0, r.read.entries);
  // Each look of the clock 60 ms on: time for two, the settle's first read of it the start.
  g_lookupStep = 60;
  r = c.read();
  TEST_ASSERT_EQUAL_UINT32(1, c.lookup.gone());
  TEST_ASSERT_EQUAL_UINT32(1, c.lookup.unasked());
  TEST_ASSERT_EQUAL_UINT32(1, r.read.entries);
  TEST_ASSERT_FALSE(c.lookup.failed());
  // Nothing past the time, no file gone: one read of the file.
  g_lookupStep = 1000;
  r = c.read();
  TEST_ASSERT_EQUAL_UINT32(0, c.lookup.gone());
  TEST_ASSERT_EQUAL_UINT32(2, c.lookup.unasked());
  TEST_ASSERT_EQUAL_UINT32(2, r.read.entries);
  c.lookup.limit(nullptr, 0);
  g_lookupStep = 0;
}

// Under the wrapper, as the firmware. A read of queue.txt with no answer, the card back at once
// (the look and its second try, docs/METADATA.md 3.8): that read's error when the second try gets
// none either, and no remount, and the boot's next read has
// the file. A question the same: the line stays by its path, and nothing mounts. A card silent past
// its init: the mark stands, and the boot's next read is FatFs's remount (at the boot, not under a
// job: QueueStore::restore()'s echoNow() is the same call).
void mendSilence(uint32_t) {
  fatmodel::Drive& d = fatmodel::drive(0);
  d.noAnswerLba = UINT32_MAX;
  d.silentForGood = false;
}
void test_on_fatfs_queue_txt_under_the_look() {
  QueueCard c;
  c.put(kQueue, bytesOf(queueText({kTrackA, kTrackB}, 1)));
  static SectorCache cache;
  TEST_ASSERT_TRUE(cache.capacity() || cache.begin(SectorCache::kDefaultEntries));
  static uint8_t scratch[fatmodel::kSS];
  fatmodel::Slot slot(0);
  fatmodel::Link link(0);
  CachedDrive wrapper(cache, slot);
  wrapper.setScratch(scratch);
  wrapper.setLink(&link);
  fatmodel::Drive& d = fatmodel::drive(0);
  d.wrapper = &wrapper;
  d.fs = &Card::fatfs;
  const auto sectorOf = [](DWORD cluster) {
    return static_cast<uint32_t>(Card::fatfs.database) + (static_cast<uint32_t>(cluster) - 2) * Card::fatfs.csize;
  };
  FIL f;
  TEST_ASSERT_EQUAL_INT(FR_OK, f_open(&f, fatmodel::path(0, ".player/queue.txt").c_str(), FA_READ));
  const uint32_t data = sectorOf(f.obj.sclust);
  f_close(&f);
  DIR dir;
  TEST_ASSERT_EQUAL_INT(FR_OK, f_opendir(&dir, fatmodel::path(0, "music/Band/Live").c_str()));
  const uint32_t folder = sectorOf(dir.obj.sclust);
  f_closedir(&dir);
  c.remount();
  TEST_ASSERT_TRUE(wrapper.remember(d.disk->sectors()));
  const auto read = [&]() {
    c.q.release();
    return queuefile::readAtBoot(c.files, kQueue, kQueueTmp, c.catalog, c.q, nullptr, nullptr, nullptr, nullptr,
                                 mendSilence, &c.lookup);
  };

  // The file's own sector.
  d.noAnswerLba = data;
  queuefile::Read r = read();
  TEST_ASSERT_TRUE(r.got == queuefile::Got::Whole);
  TEST_ASSERT_EQUAL_UINT32(2, r.reads);
  TEST_ASSERT_TRUE(r.faulted);
  TEST_ASSERT_EQUAL_UINT32(2, c.q.size());
  TEST_ASSERT_EQUAL_UINT32(1, c.lookup.kept());  // the second line, by its path: the card has it
  TEST_ASSERT_EQUAL_UINT32(0, wrapper.remounts());
  TEST_ASSERT_EQUAL_UINT32(2, wrapper.kept());  // the read and its second try: the sector still has no answer

  // A question: the folder of the line the library doesn't list.
  c.remount();
  uint32_t remounts = wrapper.remounts();
  d.noAnswerLba = folder;
  r = read();
  d.noAnswerLba = UINT32_MAX;
  TEST_ASSERT_TRUE(r.got == queuefile::Got::Whole);
  TEST_ASSERT_EQUAL_UINT32(1, r.reads);
  TEST_ASSERT_FALSE(r.faulted);
  TEST_ASSERT_EQUAL_UINT32(2, c.q.size());
  TEST_ASSERT_TRUE(c.lookup.failed());
  TEST_ASSERT_EQUAL_UINT32(1, c.lookup.unasked());
  TEST_ASSERT_EQUAL_UINT32(remounts, wrapper.remounts());
  TEST_ASSERT_EQUAL_UINT32(4, wrapper.kept());

  // The file's sector, the card silent past its init.
  c.remount();
  remounts = wrapper.remounts();
  d.noAnswerLba = data;
  d.silentForGood = true;
  r = read();
  TEST_ASSERT_TRUE(r.got == queuefile::Got::Whole);
  TEST_ASSERT_EQUAL_UINT32(2, r.reads);
  TEST_ASSERT_TRUE(r.faulted);
  TEST_ASSERT_EQUAL_UINT32(2, c.q.size());
  TEST_ASSERT_EQUAL_UINT32(remounts + 1, wrapper.remounts());
  TEST_ASSERT_FALSE(wrapper.foreign());
  TEST_ASSERT_EQUAL_UINT32(4, wrapper.kept());  // silent past its init: one try, nothing kept
}

// The reason the questions wait for the file to close: one on a card silent past its init makes
// FatFs's next call a remount, and an open file doesn't survive it. That read, on the way to a
// line the library leaves out, leaves the queue whole.
void test_on_fatfs_a_question_with_no_answer_doesnt_lose_the_read() {
  QueueCard c;
  c.put(kTrackC, bytesOf("c"));
  c.put(kQueue, bytesOf(queueText({kTrackB, kTrackC, "/music/Band/Live/09 - gone.mp3", kTrackA})));
  fatmodel::Drive& d = fatmodel::drive(0);
  c.remount();
  // The read of the file itself, then the card stops answering at the first question.
  const queuefile::Read clean = c.read();
  TEST_ASSERT_TRUE(clean.got == queuefile::Got::Whole);
  TEST_ASSERT_EQUAL_UINT32(3, clean.read.entries);
  c.remount();
  c.lookup.begin();
  bool cardFailed = false;
  ByteSource* in = c.files.open(kQueue, &cardFailed);
  TEST_ASSERT_NOT_NULL(in);
  const queuetext::Restored first = queuetext::read(*in, c.catalog, c.q, nullptr, nullptr, nullptr, nullptr, &c.lookup);
  TEST_ASSERT_FALSE(c.files.close());
  TEST_ASSERT_TRUE(first.ok);
  TEST_ASSERT_EQUAL_UINT32(4, first.entries);  // every line, the card not asked yet
  d.noInit = true;  // (what a read with no answer leaves: STA_NOINIT, FatFs's next call mounts again)
  d.failFrom = d.asked.reads;
  d.failTo = UINT64_MAX - 1;
  const bool again = c.lookup.settle();
  d.failFrom = d.failTo = UINT64_MAX;
  d.noInit = false;
  TEST_ASSERT_FALSE(again);
  TEST_ASSERT_TRUE(c.lookup.failed());
  TEST_ASSERT_EQUAL_UINT32(3, c.lookup.unasked());
  TEST_ASSERT_EQUAL_UINT32(4, c.q.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_whole_card_reads_whole_and_says_no_fault);
  RUN_TEST(test_no_root_is_missing_and_no_fault);
  RUN_TEST(test_each_read_failed_in_its_turn_is_the_cards_fault);
  RUN_TEST(test_a_card_that_fails_every_read_or_is_gone_is_unread);
  RUN_TEST(test_a_candidate_that_cant_be_read_holds_up_the_election);
  RUN_TEST(test_a_cut_or_damaged_file_is_never_the_cards_fault);
  RUN_TEST(test_a_root_read_again_forgets_the_fault_and_what_it_held);
  RUN_TEST(test_the_log_line_says_the_root_is_unread);
  RUN_TEST(test_the_boot_reads_once_unless_the_card_failed);
  RUN_TEST(test_the_boot_reads_again_while_the_card_fails_and_keeps_what_it_finds);
  RUN_TEST(test_the_boot_gives_up_after_its_reads);
  RUN_TEST(test_the_boot_waits_between_its_reads_and_not_after_the_last);
  RUN_TEST(test_on_fatfs_no_folder_and_no_file_are_its_word_not_a_fault);
  RUN_TEST(test_on_fatfs_a_damaged_directory_entry_is_the_files_fault_not_the_cards);
  RUN_TEST(test_on_fatfs_a_glitch_on_a_folders_sector_is_read_again_at_the_boot);
  RUN_TEST(test_on_fatfs_which_fil_keeps_an_error);
  RUN_TEST(test_on_fatfs_each_sector_read_failed_in_its_turn_is_never_no_root);
  RUN_TEST(test_on_fatfs_the_records_are_never_gone_for_a_read_that_failed);
  RUN_TEST(test_on_fatfs_library_idx_is_never_missing_or_corrupt_for_a_read_that_failed);
  RUN_TEST(test_on_fatfs_a_damaged_directory_entry_is_the_files_fault_at_the_boot_too);
  RUN_TEST(test_on_fatfs_an_entry_says_whose_word_it_is);
  RUN_TEST(test_on_fatfs_look_says_whose_word_it_is);
  RUN_TEST(test_on_fatfs_a_read_of_queue_txt_that_fails_is_never_no_queue);
  RUN_TEST(test_on_fatfs_a_read_that_fails_in_the_last_line_is_no_whole_file);
  RUN_TEST(test_on_fatfs_a_read_again_that_fails_says_so);
  RUN_TEST(test_on_fatfs_no_memory_to_read_queue_txt_is_unread_not_no_queue);
  RUN_TEST(test_on_fatfs_no_queue_txt_a_cut_or_damaged_one_are_no_queue);
  RUN_TEST(test_on_fatfs_queue_tmp_stands_in);
  RUN_TEST(test_on_fatfs_a_line_leaves_only_when_its_file_is_gone);
  RUN_TEST(test_on_fatfs_a_question_that_fails_keeps_the_lines_and_stops_asking);
  RUN_TEST(test_on_fatfs_a_read_asks_no_longer_than_its_time);
  RUN_TEST(test_on_fatfs_queue_txt_under_the_look);
  RUN_TEST(test_on_fatfs_a_question_with_no_answer_doesnt_lose_the_read);
  return UNITY_END();
}
