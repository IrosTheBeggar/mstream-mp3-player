// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for cardformat: what a card that didn't mount is, from
// synthetic first sectors, and the message each kind gets (UiText; their
// widths are test_ui_library's). Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "CardFormat.h"
#include "UiText.h"

namespace {
using Sector = std::vector<uint8_t>;
using cardformat::Kind;

// A card: the sectors it has; reads of any other fail, and are counted.
struct Card {
  std::map<uint32_t, Sector> sectors;
  std::vector<uint32_t> reads;
};

bool readSector(uint32_t lba, uint8_t* out, void* ctx) {
  Card& c = *static_cast<Card*>(ctx);
  c.reads.push_back(lba);
  auto it = c.sectors.find(lba);
  if (it == c.sectors.end()) return false;
  std::memcpy(out, it->second.data(), cardformat::kSectorBytes);
  return true;
}

Kind classify(Card& c) {
  uint8_t buf[cardformat::kSectorBytes];
  return cardformat::classify(readSector, &c, buf);
}

Sector blank() { return Sector(cardformat::kSectorBytes, 0); }

// A boot sector with this OEM name (and the 0x55AA every boot sector has).
Sector bootSector(const char* oem) {
  Sector s = blank();
  s[0] = 0xEB;
  s[1] = 0x76;
  s[2] = 0x90;
  std::memcpy(&s[3], oem, 8);
  s[510] = 0x55;
  s[511] = 0xAA;
  return s;
}

// A FAT32 volume's boot sector (as mkfs.fat writes one).
Sector fat32BootSector() {
  Sector s = bootSector("mkfs.fat");
  s[11] = 0x00;  // 512 bytes a sector
  s[12] = 0x02;
  std::memcpy(&s[82], "FAT32   ", 8);
  return s;
}

// An MBR with partition `slot` of `type` from `lba`.
Sector mbr(int slot, uint8_t type, uint32_t lba) {
  Sector s = blank();
  s[0] = 0xFA;  // boot code (cli), not a jump
  uint8_t* e = &s[446 + 16 * slot];
  e[4] = type;
  e[8] = static_cast<uint8_t>(lba);
  e[9] = static_cast<uint8_t>(lba >> 8);
  e[10] = static_cast<uint8_t>(lba >> 16);
  e[11] = static_cast<uint8_t>(lba >> 24);
  e[12] = 0x00;  // 1 GB of sectors: anything
  e[14] = 0x20;
  s[510] = 0x55;
  s[511] = 0xAA;
  return s;
}
}  // namespace

void setUp() {}
void tearDown() {}

// No card (every read fails): the plain "No microSD card".
void test_no_card_is_unreadable() {
  Card c;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Unreadable), static_cast<int>(classify(c)));
  TEST_ASSERT_FALSE(cardformat::notFat32(Kind::Unreadable));
  TEST_ASSERT_EQUAL_UINT32(1, c.reads.size());
}

// A card over 32 GB as it comes: an MBR, one partition of type 0x07 at
// 32,768, holding exFAT.
void test_factory_exfat_in_an_mbr() {
  Card c;
  c.sectors[0] = mbr(0, 0x07, 32768);
  c.sectors[32768] = bootSector("EXFAT   ");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::ExFat), static_cast<int>(classify(c)));
  TEST_ASSERT_TRUE(cardformat::notFat32(Kind::ExFat));
  TEST_ASSERT_EQUAL_UINT32(2, c.reads.size());
  TEST_ASSERT_EQUAL_UINT32(32768, c.reads[1]);
  TEST_ASSERT_EQUAL_STRING("exFAT", cardformat::name(Kind::ExFat));
}

// The 0x07 partition needn't be the first entry; NTFS shares the type.
void test_type_07_in_any_slot_and_ntfs() {
  Card c;
  c.sectors[0] = mbr(2, 0x07, 2048);
  c.sectors[2048] = bootSector("NTFS    ");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Ntfs), static_cast<int>(classify(c)));
  TEST_ASSERT_TRUE(cardformat::notFat32(Kind::Ntfs));
}

// exFAT straight from LBA 0, no partition table (some tools format so).
void test_exfat_at_lba_0() {
  Card c;
  c.sectors[0] = bootSector("EXFAT   ");
  // (its boot code may hold anything where an MBR's types would be)
  c.sectors[0][446 + 4] = 0xEE;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::ExFat), static_cast<int>(classify(c)));
  TEST_ASSERT_EQUAL_UINT32(1, c.reads.size());
}

// A GPT: sector 0 is the protective MBR, one partition of type 0xEE.
void test_gpt_protective_mbr() {
  Card c;
  c.sectors[0] = mbr(0, 0xEE, 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Gpt), static_cast<int>(classify(c)));
  TEST_ASSERT_TRUE(cardformat::notFat32(Kind::Gpt));
  TEST_ASSERT_EQUAL_UINT32(1, c.reads.size());  // the GPT itself isn't read
}

// FAT32 in an MBR (type 0x0C), or as a whole-card volume whose boot code
// happens to have 0xEE or 0x07 where a partition type would be: not
// recognised as anything else (that card failed to mount for another
// reason: "Can't read this card", never "format it").
void test_fat32_is_never_called_otherwise() {
  Card a;
  a.sectors[0] = mbr(0, 0x0C, 8192);
  a.sectors[8192] = fat32BootSector();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Other), static_cast<int>(classify(a)));
  TEST_ASSERT_EQUAL_UINT32(1, a.reads.size());
  Card b;
  b.sectors[0] = fat32BootSector();
  b.sectors[0][446 + 4] = 0xEE;
  b.sectors[0][462 + 4] = 0x07;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Other), static_cast<int>(classify(b)));
  TEST_ASSERT_FALSE(cardformat::notFat32(Kind::Other));
}

// A type 0x07 partition that holds something else, or can't be read; a
// sector 0 without the 0x55AA signature; a blank card.
void test_what_isnt_recognised() {
  Card a;
  a.sectors[0] = mbr(0, 0x07, 2048);
  a.sectors[2048] = bootSector("MSDOS5.0");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Other), static_cast<int>(classify(a)));
  Card b;
  b.sectors[0] = mbr(0, 0x07, 2048);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Other), static_cast<int>(classify(b)));
  Card c;
  c.sectors[0] = mbr(0, 0xEE, 1);
  c.sectors[0][510] = 0;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Other), static_cast<int>(classify(c)));
  Card d;
  d.sectors[0] = blank();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Other), static_cast<int>(classify(d)));
  Card e;
  e.sectors[0] = mbr(0, 0x07, 0);  // a start at 0 is no partition
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Other), static_cast<int>(classify(e)));
  TEST_ASSERT_EQUAL_UINT32(1, e.reads.size());
}

// NTFS straight from LBA 0 (no partition table), as exFAT can be.
void test_ntfs_at_lba_0() {
  Card c;
  c.sectors[0] = bootSector("NTFS    ");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Ntfs), static_cast<int>(classify(c)));
  TEST_ASSERT_EQUAL_UINT32(1, c.reads.size());
}

// A hybrid MBR (Boot Camp's, some Linux tools'): the 0xEE entry beside
// real ones, before or after a type 0x07: a GPT either way (its fix, an
// MBR, is what the player needs), the 0x07 partition never read.
void test_hybrid_mbr_is_gpt() {
  Card a;
  a.sectors[0] = mbr(0, 0x07, 2048);
  a.sectors[0][446 + 16 + 4] = 0xEE;
  a.sectors[2048] = bootSector("EXFAT   ");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Gpt), static_cast<int>(classify(a)));
  TEST_ASSERT_EQUAL_UINT32(1, a.reads.size());
  Card b;
  b.sectors[0] = mbr(1, 0x07, 2048);
  b.sectors[0][446 + 4] = 0xEE;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Gpt), static_cast<int>(classify(b)));
}

// Cards that answer but hold nothing the player knows: Linux's (one type
// 0x83 partition), an MBR with no partition (Windows' "Initialize disk"
// and nothing after): Other, "Can't read this card", from sector 0 alone.
void test_linux_and_empty_tables_arent_recognised() {
  Card a;
  a.sectors[0] = mbr(0, 0x83, 2048);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Other), static_cast<int>(classify(a)));
  TEST_ASSERT_EQUAL_UINT32(1, a.reads.size());
  Card b;
  b.sectors[0] = mbr(0, 0x00, 0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Other), static_cast<int>(classify(b)));
  TEST_ASSERT_EQUAL_UINT32(1, b.reads.size());
}

// Each kind its own message (uitext::cardMessage(), the empty state's and
// Try again's): none the same title or note; no card is the plain "No
// microSD card"; exFAT, NTFS and GPT name themselves; and a card nothing
// was recognised on (it may be FAT32 that failed to mount once, music and
// all) is never told to be formatted or erased.
void test_each_kind_has_its_own_message() {
  const Kind kinds[] = {Kind::Unreadable, Kind::Other, Kind::ExFat, Kind::Ntfs, Kind::Gpt};
  for (Kind a : kinds) {
    const uitext::CardMessage& m = uitext::cardMessage(a);
    TEST_ASSERT_TRUE(m.title && m.title[0] && m.lines[0] && m.lines[0][0] && m.lines[1] && m.lines[1][0]);
    TEST_ASSERT_NOT_NULL(std::strstr(m.still, ": "));  // Toast's "what: where"
    for (Kind b : kinds) {
      if (a == b) continue;
      TEST_ASSERT_TRUE(std::strcmp(m.title, uitext::cardMessage(b).title) != 0);
      TEST_ASSERT_TRUE(std::strcmp(m.still, uitext::cardMessage(b).still) != 0);
    }
  }
  TEST_ASSERT_EQUAL_STRING("No microSD card", uitext::cardMessage(Kind::Unreadable).title);
  TEST_ASSERT_NOT_NULL(std::strstr(uitext::cardMessage(Kind::ExFat).title, "exFAT"));
  TEST_ASSERT_NOT_NULL(std::strstr(uitext::cardMessage(Kind::Ntfs).title, "NTFS"));
  TEST_ASSERT_NOT_NULL(std::strstr(uitext::cardMessage(Kind::Gpt).title, "GPT"));
  TEST_ASSERT_NOT_NULL(std::strstr(uitext::cardMessage(Kind::Gpt).lines[1], "Master Boot Record"));
  const uitext::CardMessage& other = uitext::cardMessage(Kind::Other);
  for (const char* t : {other.title, other.lines[0], other.lines[1], other.still}) {
    TEST_ASSERT_NULL(std::strstr(t, "ormat it"));
    TEST_ASSERT_NULL(std::strstr(t, "rase"));
  }
  // An unknown value (a newer Kind, a bad byte): the plain message.
  TEST_ASSERT_EQUAL_STRING("No microSD card", uitext::cardMessage(static_cast<Kind>(200)).title);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_no_card_is_unreadable);
  RUN_TEST(test_factory_exfat_in_an_mbr);
  RUN_TEST(test_type_07_in_any_slot_and_ntfs);
  RUN_TEST(test_exfat_at_lba_0);
  RUN_TEST(test_gpt_protective_mbr);
  RUN_TEST(test_fat32_is_never_called_otherwise);
  RUN_TEST(test_what_isnt_recognised);
  RUN_TEST(test_ntfs_at_lba_0);
  RUN_TEST(test_hybrid_mbr_is_gpt);
  RUN_TEST(test_linux_and_empty_tables_arent_recognised);
  RUN_TEST(test_each_kind_has_its_own_message);
  return UNITY_END();
}
