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
// what waits. What the boot then does with it is test_library_update's. Run: pio
// test -e native
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
// status line's own, so a fault there leaves the root whole; any other leaves it
// unread, and never reads as a card with no transfer data.
void test_each_read_failed_in_its_turn_is_the_cards_fault() {
  for (const std::vector<uint8_t>* root : {&manifest(), &longRoot()}) {
    CutFs fs;
    fill(fs, *root);
    arm(fs, "/.mstream/", 1000000, -1);  // (counted, none failed)
    TEST_ASSERT_TRUE(readRoot(fs)->present);
    const long events = fs.faultEvents;
    TEST_ASSERT_GREATER_THAN_INT32(4, events);
    int unread = 0, checkedNoName = 0;
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
      }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(events - 1, unread, "all but the plan's open");
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
  RUN_TEST(test_on_fatfs_each_sector_read_failed_in_its_turn_is_never_no_root);
  return UNITY_END();
}
