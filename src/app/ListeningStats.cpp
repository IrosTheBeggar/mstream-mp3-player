// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/ListeningStats.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include <cstring>
#include <new>

#include "AutoDjJoin.h"
#include "LibraryUpdate.h"
#include "OggOpus.h"
#include "WallClock.h"
#include "app/BoardClock.h"
#include "app/Psram.h"
#include "storage/SectorDisk.h"
#include "StatsRank.h"

// The resident set (docs/LISTENING-STATS.md 5.14, "Moved to internal
// RAM"): the glue's object alone, in internal RAM (.bss, below), made once
// in setup and never destroyed; internal RAM holds nothing else of the
// stats', whatever the card (MARK's block, a card past 8 MARK rows, is
// PSRAM's: begin()). PSRAM holds nothing of the stats' between calls but
// the boot's scan's blocks (and that block); those it holds across passes
// (the hold: the ranges' block, a full log's overflow arena) sit at the top
// of the largest free block with the free block left above each closed by
// caps (topAlloc()), so main's own carves made meanwhile go where main puts
// them where the caps close it (the take's [listen] line says when they
// don't: a missed cap, no spacers, a hole, the arena's fallback, a
// decoder's room, the ranges' block's at every take and the arena's while a
// track plays: the caps given back); PSRAM's layout is then main's once the
// hold ends, in the host's model and on the device (5.14, fix A: runs C1,
// C1-V and C2, 2026-11-18; and under #64's 4,095 B malloc limit, a File's
// 4 KB stdio buffer in PSRAM, C4 and visit A against main 987216f,
// 2026-11-19, a track's buffer opened inside the hold among them; the
// device's B1 runs had found it split for the session without the caps). Its
// budget is internal RAM's margin (11's Internal RAM row): 2,048 B, hence
// 40 records, not the design's 96 (statsfold::kRecords). Against G5's
// floors (docs/OPUS.md: steady 50 KB, the session's lowest 42 KB, the
// headphones linked) what counts is the build's .dram0.bss growth over
// main's (2,344 B over S0's: this object's 2,008 B, the rework's 96 B of
// statics, Ht's 8 B and the hold's 232 B, s_tl, s_top and the room's flag
// in their fill; 2,096 B as moved), not sizeof alone. At the figures held
// with a link (METADATA 3.8, 2026-11-04) G5's steady floor was expected to
// hold with 0-1 KB to spare and the session's floor not shown kept; the
// move's re-run with the headphones linked (5.14, "Its device re-run")
// measured both held with about 6 KB to spare at 2,096 B, the link first
// and a real stream. At 2,344 B, main 987216f merged (5.14, "Its device
// runs", 2026-11-19), G5's play-before-link order read 50,764 B on a real
// stream (main 52,872 B), the figure the session's floor counts (the
// owner's call), and 42,808 B in silent mode (main 45,140 B), 200 B under,
// recorded beside it.
static_assert(sizeof(statsglue::Glue) <= 2048, "the listening stats' resident set is at most 2,048 B (5.14, 11)");

namespace {

namespace cc = cardcontract;
namespace sf = statsfold;
namespace sx = statsfile;
namespace sg = statsglue;

using ul = unsigned long;
using ull = unsigned long long;

// The boot's waits between reads the card failed (nothing else is held up
// yet: setup()).
void bootWait(uint32_t ms) { delay(ms); }

float msSince(int64_t t0) { return static_cast<float>(esp_timer_get_time() - t0) / 1000.0f; }

size_t psramLargest() { return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); }
size_t psramFreeBytes() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

// The glue's object: internal DRAM's .bss (not EXT_RAM_BSS_ATTR, not the
// heap). Static rather than an internal-heap carve in setup: it is never in
// the heap, so it never splits a free block another internal carve would
// have taken (the card worker's 7.5 KB stack at each burst, A2DP's buffers,
// a track's 4.1 KB of decoder buffers), and it can't fail. It does shrink
// the DRAM heap above .bss by its size (that heap starts at _heap_start,
// .dram0.bss's end; 79,040 B to 0x3FFE0000 in this build, 2,344 B less than
// S0's), and heap_caps_malloc() tries the internal heaps in address order,
// DRAM's before the D/IRAM ones above it: a carve that fit at that heap's
// end on main can go to the next internal heap instead, the same class of
// effect as the device's FLAC row in PSRAM (5.14). The re-run records the
// internal largest= and the card worker's lowest beside main's. Its cost,
// the same sizeof at every boot, card or none, shows in the build's RAM
// figure.
alignas(statsglue::Glue) uint8_t s_glue[sizeof(statsglue::Glue)];

// The framework's heap (ESP-IDF 5.5's TLSF; sdkconfig's light poisoning):
// each request 12 B larger to TLSF, 8 B of it before the bytes malloc gives
// (a canary and the size) and 4 B after; heap_caps_walk() reports a block by
// its TLSF pointer, the one before those 8 B, and its TLSF size (the
// poisoning and the size class's rounding in, the 4 B size field out).
#if CONFIG_HEAP_POISONING_LIGHT || CONFIG_HEAP_POISONING_COMPREHENSIVE
constexpr size_t kPoisonHead = 8, kPoisonBytes = 12;
#else
constexpr size_t kPoisonHead = 0, kPoisonBytes = 0;
#endif
// The walk's bound before it meets the block (past it: Close::Lost).
constexpr uint32_t kWalkVisits = 8192;

// autodj::AboveFn over PSRAM's heap: heap_caps_walk() hands walkStep()
// each block, and autodj::WalkAbove (lib/core, host-tested) decides. The
// callback runs inside the PSRAM heap's lock (multi_heap_walk:
// xPortEnterCriticalTimeout, this core's interrupts masked to level 3; a
// PSRAM heap call on the other core spins, its own masked, until the walk
// ends, as under any heap_caps_get_largest_free_block()): it compares and
// copies words, nothing else (no allocation, no line, no heap_caps_*, no
// timing). The state is on the caller's frame.
bool walkStep(walker_heap_into_t heap, walker_block_info_t b, void* user) {
  return static_cast<autodj::WalkAbove*>(user)->step(heap.start, reinterpret_cast<uintptr_t>(b.ptr), b.size, b.used);
}

void psramAbove(const void* block, const void* spacer, autodj::Above* out) {
  autodj::WalkAbove w(block, spacer, kPoisonHead, kWalkVisits, out);
  const int64_t t0 = esp_timer_get_time();
  heap_caps_walk(MALLOC_CAP_SPIRAM, walkStep, &w);
  out->us = static_cast<uint32_t>(esp_timer_get_time() - t0);
}

// The stats' hold (Memory::top and topRelease: the boot's scan's ranges'
// block and a full log's overflow arena, held across passes): each at the
// top of PSRAM's largest free block, the join's spacers under it while it is
// carved, never less than this free with it carved (Auto DJ's
// kJoinMarginBytes, for the same reason: another task's block may come in
// that window, a track start's 4 KB probe, a seek's 8 KB, the Opus packet
// buffer's 60 KB, the card worker's 4-16 KB, and since #64's 4,095 B malloc
// limit a File's 4 KB stdio buffer or an SDP search's list); then the free
// block the spacers left above it (T) closed by caps, held with it, so that
// a long-lived carve made while it is held (the first frame's cover canvas,
// a queue, a track's stdio buffer) goes where main puts it, not into T,
// where it would pin the largest free block split once the hold ends (5.14,
// fix A; the device's B1 runs; with #64's limit, C4 and visit A). Given
// back bottom-up, the newest first (autodj::TopHold): no free
// block opens above a held one. Never for a block held for the session
// (its caps would be): MARK's is plain psramAlloc (fix G).
constexpr size_t kTopMarginBytes = 64 * 1024;
const autodj::TopHold::Hooks kTop = {psramLargest, psramFreeBytes,  psramAlloc,  psramFree,
                                     psramAbove,   kTopMarginBytes, kPoisonBytes};
autodj::TopHold s_top;
// A decoder may open while the hold is held, and the caps keep the margin
// free, not its blocks. So a take asks the largest decoder's room
// (statsglue::kDecoderRoomBytes, the Opus's buffers) when one may open
// before the block is given back: the hold's caps given back when the
// largest free block wouldn't take them, and the overflow's arena too (its
// chunks then one by one, as before the caps: they take the room a piece at
// a time). Whether a take asks is statsglue::decoderRoom(), which the
// host's model calls too. The ranges' block asks at every take: while it
// alone is held (before the overflow's first chunk: a usual log, or a full
// log's first 40 records) main.cpp's OutputHold holds no start, so a Play
// from a stop or a pause opens its decoder under it. The arena and a chunk
// ask only while a track plays (ListeningStats::pass(): the next track's
// open, or a skip, is never held): while chunks are held OutputHold holds
// every other start, and the arena and its chunks are given back in one
// call, so a start waiting at their take opens nothing under them. (The
// arena is given back mid-scan only by its own NoRoom, which gives the
// ranges' caps back first; its chunks then hold every start. Else it goes
// at the fold's end with the ranges' block, or later, the ranges' block
// waiting for it.)
bool s_trackPlaying = false;
static_assert(sg::kDecoderRoomBytes == oggopus::kPageBytes + oggopus::kMaxPacketBytes + oggopus::kFramePacketBytes,
              "the hold's decoder room is the Opus's three buffers (OpusGenerator::allocBuffers())");

// A take's [listen] line, owed (printed at the next pass by owedLine(): a
// take may be deep in the scan's frames, the arena's under Journal::scan).
struct TopLine {
  const void* at = nullptr;
  uint32_t bytes = 0;
  autodj::CloseReport r;
};
struct TopLines {
  // Three takes at most between passes: the ranges' block, then the
  // overflow's arena or, no room for it, the arena refused and its first
  // chunk alone (a slot's carve: kReserveAhead is 0). Never overwritten: a
  // take past them is counted (lost).
  TopLine line[3];
  uint32_t blocks = 0;       // (the boot's first take) PSRAM's blocks, heap_caps_get_info()'s total_blocks
  uint32_t largestUs = 0;    // ... one heap_caps_get_largest_free_block()'s time: a whole walk under the same lock
  uint16_t outside = 0;      // takes past the hold's two blocks (Full: the fallback's chunks after its first)
  uint16_t outsideSeen = 0;  // ... as the last pass left it: one line once a pass brings none
  uint8_t n = 0, lost = 0;
  uint8_t uncapped = 0;      // the hold's caps given back for the fallback's chunks (told on a chunk's line)
  bool counted = false, firstOwed = false;
};
TopLines s_tl;

void* topAlloc(size_t bytes) {
  if (!s_tl.counted) {
    s_tl.counted = s_tl.firstOwed = true;
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    s_tl.blocks = static_cast<uint32_t>(info.total_blocks);
    const int64_t t0 = esp_timer_get_time();
    (void)psramLargest();
    s_tl.largestUs = static_cast<uint32_t>(esp_timer_get_time() - t0);
  }
  // The overflow's chunks one by one (no room for its arena: Fold::
  // carveChunk()) are carved as before the caps: under spacers keeping 64 KB
  // free, nothing closed, the hold's caps given back first, so the chunks
  // find the room the caps held (an arena that can't be carved behaves as
  // chunks that couldn't be before it; the ranges' block, still held, waits
  // behind the first chunk). kChunkBytes is no other take's size: the arena
  // is 262,080 B, the ranges' request the fold's need + 4 B, need a multiple
  // of 16 (112 B an ACKS row, 80 B a MARK row's ranges: 8 B a log file).
  const bool chunk = bytes == sf::kChunkBytes, arena = bytes == sf::kArenaBytes;
  if (chunk) s_tl.uncapped = static_cast<uint8_t>(s_tl.uncapped + s_top.uncap(kTop));
  autodj::CloseReport r;
  void* p = s_top.take(kTop, bytes, &r, !chunk, sg::decoderRoom(bytes, s_trackPlaying), arena);
  if (r.why == autodj::Close::Full) {
    ++s_tl.outside;
    return p;
  }
  if (s_tl.n == 3) {
    ++s_tl.lost;
    return p;
  }
  TopLine& l = s_tl.line[s_tl.n++];
  l.at = p;
  l.bytes = static_cast<uint32_t>(bytes);
  l.r = r;
  return p;
}

void topRelease(void* p) { s_top.give(kTop, p); }

// S2's asks (app/StatsLists: Hr, Hl, Hw, Hp): one block an ask, held across
// passes (the worker's step, the walk), so at the top as the boot hold's,
// in the same TopHold (an ask waits for the boot's scan to end, and the
// boot's scan of a remount waits while an ask holds its block: never both).
// A decoder may open while it is held (a start, the next track): its room
// asked at every take, as for the ranges' block; with no room the block
// yields (NoRoom: the ask refused, its line says so).
void* listsTake(size_t bytes, autodj::CloseReport* r) {
  return s_top.take(kTop, bytes, r, true, sg::kDecoderRoomBytes, true);
}
void listsGive(void* p) { s_top.give(kTop, p); }

// The owed lines (owedLine(): at a pass's start, the loop's own frame).
__attribute__((noinline)) void printTopLines() {
  for (uint8_t i = 0; i < s_tl.n; ++i) {
    const TopLine& l = s_tl.line[i];
    const autodj::CloseReport& r = l.r;
    // (Named by size, as topAlloc() tells them: the arena 262,080 B, a chunk
    // 16,384 B, the ranges' block anything else.)
    const bool chunk = l.bytes == sf::kChunkBytes;
    const char* which = l.bytes == sf::kArenaBytes ? "the overflow's arena"
                        : chunk                    ? "an overflow chunk (no room for the arena)"
                                                   : "the ranges'";
    char uncapped[48] = "";
    if (chunk && s_tl.uncapped) {
      snprintf(uncapped, sizeof(uncapped), "; the hold's %u cap%s given back first", static_cast<unsigned>(s_tl.uncapped),
               s_tl.uncapped == 1 ? "" : "s");
      s_tl.uncapped = 0;
    }
    if (r.why == autodj::Close::NoBlock) {
      Serial.printf("[listen] the hold's top block (%s): NO PSRAM for its %lu B (no free block takes it; the fold's "
                    "own lines say what follows)%s\n",
                    which, static_cast<ul>(l.bytes), uncapped);
      continue;
    }
    // (Why a decoder may open in the hold: the arena's room is asked only
    // while a track plays, the ranges' at every take.)
    const char* may = l.bytes == sf::kArenaBytes ? "a track playing" : "a start or a track playing";
    if (r.why == autodj::Close::NoRoom) {
      Serial.printf("[listen] the hold's top block (%s): its %lu B GIVEN BACK at once: with it held no free block "
                    "takes a decoder's %lu B (a track playing), the hold's %u caps given back first; its chunks one "
                    "by one, as before the caps%s\n",
                    which, static_cast<ul>(l.bytes), static_cast<ul>(sg::kDecoderRoomBytes),
                    static_cast<unsigned>(r.uncapped), uncapped);
      continue;
    }
    const ul f = static_cast<ul>(r.aboveBytes), n = r.caps, cb = static_cast<ul>(r.capBytes);
    char kept[48] = "";
    if (n) snprintf(kept, sizeof(kept), "; %lu cap%s kept (%lu B)", n, n == 1 ? "" : "s", cb);
    char what[200];
    switch (r.why) {
      case autodj::Close::Closed:
        snprintf(what, sizeof(what), "the %lu B free above it closed by %lu cap%s (%lu B)", f, n, n == 1 ? "" : "s",
                 cb);
        break;
      case autodj::Close::Nothing:
        if (r.usedAbove) {
          snprintf(what, sizeof(what),
                   "above it a block in use (%lu B; a 4 KB probe or a FatFile is 4,224 B): nothing to close",
                   static_cast<ul>(r.usedAbove));
        } else if (f) {
          snprintf(what, sizeof(what), "the %lu B free above it too small for any request: nothing to close", f);
        } else {
          snprintf(what, sizeof(what), "above it PSRAM's end: nothing to close");
        }
        break;
      case autodj::Close::NoSpacers:
        snprintf(what, sizeof(what), "no spacers placed (PSRAM too full or too cut up): nothing closed");
        break;
      case autodj::Close::NotOnTop:
        snprintf(what, sizeof(what), "in a smaller free block, not on the spacers' top: nothing closed");
        break;
      case autodj::Close::Margin:
        snprintf(what, sizeof(what), "the %lu B free above it LEFT OPEN: closing it would leave under %lu KB free", f,
                 static_cast<ul>(kTopMarginBytes / 1024 + 1));
        break;
      case autodj::Close::Missed:
        snprintf(what, sizeof(what),
                 "the %lu B free above it LEFT OPEN: a cap came back at 0x%08lx, not 0x%08lx (another block heads its "
                 "list, or another task's carve came first), given back%s",
                 f, static_cast<ul>(reinterpret_cast<uintptr_t>(r.got)),
                 static_cast<ul>(reinterpret_cast<uintptr_t>(r.want)), kept);
        break;
      case autodj::Close::NoCap:
        snprintf(what, sizeof(what), "the %lu B free above it LEFT OPEN: no PSRAM for a cap%s", f, kept);
        break;
      case autodj::Close::Lost:
        snprintf(what, sizeof(what), "LEFT OPEN: the walk didn't meet it (%lu blocks%s)", static_cast<ul>(r.visits),
                 r.bounded ? ", its bound" : "");
        break;
      case autodj::Close::Left:
        snprintf(what, sizeof(what),
                 "the %lu B free above it: %lu cap%s (%lu B), the rest LEFT OPEN (the caps' most, or the block "
                 "above grew: another task's block freed into it, or another task's block took the rest's head)",
                 f, n, n == 1 ? "" : "s", cb);
        break;
      case autodj::Close::Room:
        // (Caps given back, this block's or only the older block's.)
        if (n)
          snprintf(what, sizeof(what),
                   "the %lu B free above it LEFT OPEN: with the hold's caps held no free block takes a decoder's "
                   "%lu B (%s): the hold's %u caps given back, its %lu among them",
                   f, static_cast<ul>(sg::kDecoderRoomBytes), may, static_cast<unsigned>(r.uncapped), n);
        else
          snprintf(what, sizeof(what),
                   "with the hold's caps held no free block takes a decoder's %lu B (%s): the hold's %u caps given "
                   "back, none of them its own",
                   static_cast<ul>(sg::kDecoderRoomBytes), may, static_cast<unsigned>(r.uncapped));
        break;
      case autodj::Close::Plain:
        snprintf(what, sizeof(what), "carved as before the caps (64 KB kept free under its spacers): nothing closed");
        break;
      default:
        what[0] = 0;
        break;
    }
    // (The walks' clause only for a take that walked: NoSpacers and Plain
    // takes have no block size from a walk; a Lost one walked but never met
    // its block, so the walk gave it none.)
    char walks[112] = "not walked";
    if (r.walks) {
      char carved[24] = "the block not met";
      if (r.blockBytes) snprintf(carved, sizeof(carved), "%lu B carved", static_cast<ul>(r.blockBytes));
      snprintf(walks, sizeof(walks), "%s; %lu walk%s in %lu us, the longest %lu us, %lu blocks", carved,
               static_cast<ul>(r.walks), r.walks == 1 ? "" : "s", static_cast<ul>(r.us), static_cast<ul>(r.usMax),
               static_cast<ul>(r.visits));
    }
    char first[80] = "";
    if (i == 0 && s_tl.firstOwed)
      snprintf(first, sizeof(first), "; PSRAM %lu blocks, a largest-free-block walk %lu us",
               static_cast<ul>(s_tl.blocks), static_cast<ul>(s_tl.largestUs));
    Serial.printf("[listen] the hold's top block (%s): %lu B at 0x%08lx; %s (%s)%s%s\n", which,
                  static_cast<ul>(l.bytes), static_cast<ul>(reinterpret_cast<uintptr_t>(l.at)), what, walks, uncapped,
                  first);
  }
  if (s_tl.n) s_tl.firstOwed = false;
  if (s_tl.lost)
    Serial.printf("[listen] the hold's top blocks: %u more taken before this pass, not told\n",
                  static_cast<unsigned>(s_tl.lost));
  // The takes past the hold's two blocks: one line once they stop coming (a
  // pass that brought none).
  if (s_tl.outside && s_tl.outside == s_tl.outsideSeen) {
    Serial.printf("[listen] the hold's top blocks: %u more overflow chunk%s (no room for the arena) carved as before "
                  "the caps past the hold's two blocks: not tracked, nothing closed, each freed when given back\n",
                  static_cast<unsigned>(s_tl.outside), s_tl.outside == 1 ? "" : "s");
    s_tl.outside = 0;
  }
  s_tl.outsideSeen = s_tl.outside;
  s_tl.n = s_tl.lost = 0;
}

// "7 h 37 min", "45 min" (4.1's times).
void listened(uint64_t ms, char* out, size_t cap) {
  const uint64_t min = ms / 60000;
  if (min >= 60) {
    snprintf(out, cap, "%llu h %llu min", static_cast<ull>(min / 60), static_cast<ull>(min % 60));
  } else {
    snprintf(out, cap, "%llu min", static_cast<ull>(min));
  }
}

// A UTC day since 2026-01-01 as "2026-11-08".
void dayText(uint32_t day, char* out, size_t cap) {
  const wallclock::Civil c = wallclock::toCivil(sf::kMinMs + static_cast<uint64_t>(day) * sf::kDayMs);
  snprintf(out, cap, "%04d-%02d-%02d", c.year, c.month, c.day);
}

// A row's time (seconds since 2026-01-01, plus 1) as "2026-11-08 21:40:00".
void timeText(uint32_t t, char* out, size_t cap) {
  if (t == 0 || t == sf::kNoFirst) {
    snprintf(out, cap, "%s", t == 0 ? "none" : "none (skips only)");
    return;
  }
  wallclock::text(wallclock::toCivil(sf::kMinMs + static_cast<uint64_t>(t - 1) * 1000), out, cap);
}

void devText(const uint8_t d[6], char* out, size_t cap) {
  snprintf(out, cap, "%02x%02x%02x%02x%02x%02x", d[0], d[1], d[2], d[3], d[4], d[5]);
}

// Ht's 16 hex digits, a track's path hash; false: not 16 hex digits.
bool hashOf(const char* hex, uint64_t* out) {
  uint64_t h = 0;
  int n = 0;
  for (; hex[n]; ++n) {
    const char c = hex[n];
    const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (v < 0 || n == 16) return false;
    h = h << 4 | static_cast<uint64_t>(v);
  }
  if (n != 16) return false;
  *out = h;
  return true;
}

// A section's type ("TRKD"; 0: stats.bin's frame).
void typeText(uint32_t t, char* out, size_t cap) {
  if (!t) {
    snprintf(out, cap, "its frame");
    return;
  }
  snprintf(out, cap, "%c%c%c%c", static_cast<char>(t), static_cast<char>(t >> 8), static_cast<char>(t >> 16),
           static_cast<char>(t >> 24));
}

const char* srcName(uint8_t s) { return s == sf::kAutoDj ? "Auto DJ" : s == sf::kShuffle ? "shuffle" : "you"; }

const char* settledText(tagstore::Settle s) {
  switch (s) {
    case tagstore::Settle::Clean: return "";
    case tagstore::Settle::Removed: return "; a leftover stats.tmp removed";
    case tagstore::Settle::Promoted: return "; stats.tmp promoted (a cut save's: stats.bin was gone)";
    case tagstore::Settle::Quarantined: return "; stats.tmp shared a chain: renamed to a twin (a disk check frees it)";
    case tagstore::Settle::Failed: return "; stats.tmp couldn't be settled (a remove or a rename the card refused)";
    case tagstore::Settle::Kept: return "; stats.tmp kept (it may be the only copy)";
    case tagstore::Settle::Unread: return "; the card failed a question of stats.tmp";
  }
  return "";
}

const char* refusedText(sg::Refused r) {
  switch (r) {
    case sg::Refused::None: return "";
    case sg::Refused::Off: return "the stats are off this session";
    case sg::Refused::Ending: return "the orderly end flushes (its 3 s are the plays'; the next boot folds again)";
    case sg::Refused::Budget:
      return "under 1 s of the end's flush was left: the plays of the plays.1.jnl this rotation takes out are lost "
             "to the stats";
    case sg::Refused::CardAway: return "the card is away (out of the slot, refusing writes, or another card in it)";
    case sg::Refused::Partial: return "a log file wasn't read whole this session (the next boot folds again)";
    case sg::Refused::Booting: return "the boot's scan (or its save, tried again) is under way";
    case sg::Refused::Fenced: return "the library update's fence is up";
    case sg::Refused::Behind:
      return "this session lacks slots the log holds above the card's MARK (the records full, a save that didn't "
             "land with the boot's overflow, a held stats.tmp lost, or a section damaged): no save this session; the "
             "next boot folds what the log still holds (a rotation meanwhile loses those in plays.1.jnl)";
  }
  return "";
}

// (PlayHistory's own: the card isn't one to ask now.)
bool away() {
  if (!sectordisk::installed()) return false;
  const sectordisk::Guard g = sectordisk::guard();
  return sectordisk::cardOut() || g.writesOff || g.foreign;
}

}  // namespace

playlog::Folds* ListeningStats::begin(tagstore::Fs& fs, const cardroot::Root& root) {
  fs_ = &fs;
  hasCommit_ = root.hasRoot && root.commitTime != 0;
  commitSec_ = root.commitTime;
  if (glue_) return glue_;  // (once a boot: the object is never made again)
  // Every PSRAM block the stats carve is given back in its call (a read's
  // or a save's work memory and FatFiles) or by the boot's scan's end (the
  // ranges' block; an overflow's arena once its save lands, or after its
  // last try: the hold, topAlloc()): nothing of the stats' sits in PSRAM
  // between them but MARK's block. That one (statsfile::Store, a card with
  // more than 8 MARK rows: its rows and 8 more, 28 B a row, as many as the
  // card holds) is carved by this boot's read, in setup, and held for the
  // session: plain psramAlloc, where any session block goes (5.14's fix G:
  // at a top it would hold its cap for the session too). Not internal
  // RAM's: its size is the card's, with no bound (MARK's rows are never
  // dropped; each player and each NVS erase adds one), and G5's margin has
  // none to give (the internal-RAM review). A card of 8 rows or fewer
  // carves none.
  sg::Memory m;
  m.alloc = psramAlloc;
  m.top = topAlloc;
  m.topRelease = topRelease;
  m.release = psramFree;
  m.keep = psramAlloc;
  m.keepRelease = psramFree;
  glue_ = new (s_glue) sg::Glue(fs, static_cast<sg::Env&>(*this), m);
  StatsLists::Hold hold;
  hold.take = listsTake;
  hold.give = listsGive;
  lists_.begin(glue_, &fs, hold, printTrackLook);
  const int64_t t0 = esp_timer_get_time();
  const sx::Opened& o = glue_->begin(root.acks, bootWait);
  logRead(o, "at the boot", msSince(t0));
  return glue_;
}

// ---------------------------------------------------------------------------
// The passes.
// ---------------------------------------------------------------------------
void ListeningStats::pass(const Pass& p, uint32_t nowMs) {
  if (!glue_) return;
  if (!seeksKnown_ || p.seeks != seeks_) {
    if (seeksKnown_) {
      seekSeen_ = true;
      seekAt_ = nowMs;
    }
    seeksKnown_ = true;
    seeks_ = p.seeks;
  }
  if (seekSeen_ && nowMs - seekAt_ >= LibraryUpdate::kSeekQuietMs) seekSeen_ = false;
  playing_ = p.playing;
  waiting_ = p.waiting;
  s_trackPlaying = p.playing;
  safePoint_ = LibraryUpdate::safePoint(p.playing, p.waiting, p.trackLeftMs, seekSeen_ ? nowMs - seekAt_ : UINT32_MAX);
  joinWorking_ = p.joinWorking;
  updating_ = p.updating;
}

bool ListeningStats::mayScan() {
  if (!glue_) return true;
  // An S2 ask holds its block at the top (a remount's scan: an ask waits
  // for the boot's): the scan's pieces wait for it, a pass at a time.
  if (lists_.holds()) return false;
  switch (glue_->scanWaits(updating_, joinWorking_)) {
    case sg::ScanWait::Update:
      // A library update asked and not over, the boot's fold not begun:
      // neither it nor a remount's read begins behind its memory check or
      // during its build (the plays wait in the Tracker). Once begun, the
      // update waits for the scan's blocks instead (Wait::Stats).
      ++updateWaits_;
      return false;
    case sg::ScanWait::Join:
      // The boot's fold carves only in a pass where Auto DJ's join isn't
      // Working (5.6); so does a remount's read of stats.bin.
      ++scanWaits_;
      return false;
    case sg::ScanWait::None: break;
  }
  if (glue_->wantsRoom() && !glue_->reserve(sg::kReserveAhead)) ++reserveFailed_;
  return true;
}

void ListeningStats::scanAway() {
  if (!glue_) return;
  const size_t ranges = glue_->rangesHeld() ? glue_->rangesBytes() : 0;
  const uint32_t chunks = glue_->fold().chunks();
  if (!glue_->scanAway()) return;
  Serial.printf("[listen] the card went away during the boot's scan: its fold ended as a partial scan's (the ranges' "
                "block %lu B and %lu overflow chunk%s given back%s): no save this session unless the card mounts "
                "again (the scan then again from its start); the trim by the MARK read at the boot\n",
                static_cast<ul>(ranges), static_cast<ul>(chunks), chunks == 1 ? "" : "s",
                glue_->fold().behind() ? "; past the records: behind" : "");
}

void ListeningStats::ready(uint32_t passes, int64_t scanUs) {
  if (!glue_ || readyLogged_) return;
  readyLogged_ = true;
  glue_->ready();
  const sf::Fold& f = glue_->fold();
  if (!glue_->on()) {
    Serial.printf("[listen] the boot's scan done (%lu passes, %.0f ms on the loop): the stats are OFF this session "
                  "(%s): nothing folded, saved or trimmed by them\n",
                  static_cast<ul>(passes), static_cast<float>(scanUs) / 1000.0f,
                  glue_->dropped() ? "the folds dropped at the end" : sx::readName(glue_->opened().read));
    return;
  }
  char more[240] = "";
  if (glue_->partialSeen()) {
    snprintf(more, sizeof(more), "; a log file not read whole: NO save this session, the trim by the MARK read at "
                                 "the boot, the next boot folds again%s",
             f.behind() ? " (its overflow dropped: behind)" : "");
  } else if (f.noMemory()) {
    snprintf(more, sizeof(more), "; the fold ran OUT OF MEMORY: no save this session, the trim by the MARK read at "
                                 "the boot");
  } else if (glue_->retrying()) {
    const uint64_t now = boardclock::monoMs(), at = glue_->retryAtMs();
    snprintf(more, sizeof(more), "; its save didn't land: the overflow held (%lu chunks), tried again in %lu ms (try "
                                 "%lu of %lu), a track's start and a library update waiting meanwhile",
             static_cast<ul>(f.chunks()), static_cast<ul>(at > now ? at - now : 0),
             static_cast<ul>(glue_->tries() + 1), static_cast<ul>(sg::kBootTries));
  } else if (f.behind()) {
    snprintf(more, sizeof(more), "; BEHIND its log (%s): no save this session",
             glue_->lastRefused() != sg::Refused::None ? sg::refusedName(glue_->lastRefused()) : "its save didn't land");
  }
  Serial.printf("[listen] the boot's fold: %lu slots folded (%lu met twice, gap %lu) in the scan's %lu passes (%.0f ms "
                "on the loop%s%s%s); the ranges' block %lu B; the overflow %lu B at the peak (%lu chunks now); "
                "%lu records, MARK %lu rows%s\n",
                static_cast<ul>(f.folded()), static_cast<ul>(f.twice()), static_cast<ul>(f.gap()),
                static_cast<ul>(passes), static_cast<float>(scanUs) / 1000.0f,
                scanWaits_ ? ", some waiting for Auto DJ's join" : "",
                updateWaits_ ? ", its first waiting for a library update" : "",
                reserveFailed_ ? ", some pieces' room not taken first" : "", static_cast<ul>(glue_->rangesBytes()),
                static_cast<ul>(f.peak()), static_cast<ul>(f.chunks()), static_cast<ul>(f.records()),
                static_cast<ul>(f.marks()), more);
}

void ListeningStats::written(const playlog::Slot& s) {
  if (glue_) glue_->written(s);
}

void ListeningStats::ending(uint64_t untilMs) {
  // (An S2 ask's step on the card worker ends first: no rotation's save
  // replaces stats.bin under its reads. Its block given back.)
  if (untilMs == 0) lists_.ending(3000);
  if (glue_) glue_->ending(untilMs);
}

void ListeningStats::runSave(bool fenced) {
  if (!glue_) return;
  const bool retry = glue_->retryDue();
  glue_->saveNow(fenced);  // (its line: saved())
  if (!retry) return;
  if (glue_->retrying()) {
    const uint64_t now = boardclock::monoMs(), at = glue_->retryAtMs();
    Serial.printf("[listen] the boot's overflow held: its save tried again in %lu ms (try %lu of %lu)\n",
                  static_cast<ul>(at > now ? at - now : 0), static_cast<ul>(glue_->tries() + 1),
                  static_cast<ul>(sg::kBootTries));
  } else if (glue_->fold().behind()) {
    Serial.printf("[listen] the boot's overflow given back after %lu tries of its save: the session is BEHIND its "
                  "log (no save this session; the next boot folds what the log still holds, a rotation meanwhile "
                  "loses those in plays.1.jnl)\n",
                  static_cast<ul>(glue_->tries()));
  }
}

void ListeningStats::runHs(bool fenced) {
  hsWaiting_ = false;
  if (glue_) glue_->hs(fenced);  // (its line: saved())
}

// ---------------------------------------------------------------------------
// statsglue::Env
// ---------------------------------------------------------------------------
uint64_t ListeningStats::monoMs() { return boardclock::monoMs(); }

bool ListeningStats::cardAway() { return away(); }

void ListeningStats::asking(sg::Kind k, sx::SaveAsk* ask) {
  (void)k;
  ask->nowMs = boardclock::now();  // (0 while the clock is unset)
  ask->hasCommit = hasCommit_;
  ask->commitSec = commitSec_;
  // (A play waiting, for the headphones or the stats' overflow, or a
  // restart asked, is playing for a compaction: its start, or the restart,
  // mustn't wait behind one. Not a safe point either:
  // LibraryUpdate::safePoint().)
  ask->playing = playing_ || waiting_;
  ask->safePoint = safePoint_;
  saveT0_ = esp_timer_get_time();
}

void ListeningStats::began(bool again, const sx::Opened* read) {
  readyLogged_ = false;
  if (!again) return;  // (the boot's read: begin()'s line)
  if (!read) {
    Serial.println("[listen] the card mounted again, and the end flushes: stats.bin NOT read again; the folds "
                   "dropped, nothing more saved this session (the next boot folds again)");
    return;
  }
  logRead(*read, "again, the card mounted again (one read)", 0.0f);
}

void ListeningStats::partialFile() {
  Serial.println("[listen] a log file wasn't read whole (the card failed an open or a read): no save this session; "
                 "the trim by the MARK read at the boot, the next boot folds again");
}

void ListeningStats::saved(sg::Kind k, sg::Refused r, const sx::SaveReport& rep) {
  const float ms = r == sg::Refused::None || rep.what != sx::Saved::Off ? msSince(saveT0_) : 0.0f;
  lastMs_ = ms;
  if (k == sg::Kind::Rotation) {
    // A rotation's save runs under the journal's append (writeOne, append,
    // put, place, rotate), the loop's deepest path (docs/LISTENING-STATS.md
    // 11): its line waits for the next pass (owedLine()), so printf's frames
    // (about 1.4 KB, the float's among them) are never under it.
    owed_ = true;
    owedMs_ = ms;
    return;
  }
  if (r == sg::Refused::None || rep.what == sx::Saved::Damaged || rep.what == sx::Saved::Behind) {
    logSaved(k, rep, ms);
    return;
  }
  // (No buffer on this frame: it is under the save's caller.)
  const char* off = r == sg::Refused::Off ? (glue_ && glue_->dropped() ? " (the folds dropped at the end)" : "") : "";
  Serial.printf("[listen] NOT saved (%s): %s%s\n", sg::kindName(k), refusedText(r), off);
}

void ListeningStats::owedLine() {
  printTopLines();  // (the hold's takes: a line each)
  if (!owed_) return;
  owed_ = false;
  // (Nothing saved since: one thing of the card's a pass, and the end's
  // flush writes slots alone once its scan is done. The glue's last save is
  // the rotation's, its report and refusal.)
  if (!glue_ || glue_->lastKind() != sg::Kind::Rotation) return;
  const sg::Refused r = glue_->lastRefused();
  const sx::SaveReport& rep = glue_->last();
  if (r == sg::Refused::None || rep.what == sx::Saved::Damaged || rep.what == sx::Saved::Behind) {
    logSaved(sg::Kind::Rotation, rep, owedMs_);
    return;
  }
  const char* off = r == sg::Refused::Off ? (glue_->dropped() ? " (the folds dropped at the end)" : "") : "";
  Serial.printf("[listen] NOT saved (rotation, before plays.1.jnl leaves the log): %s%s\n", refusedText(r), off);
}

// (A frame of its own, never under the store's save, nor under a rotation's
// at all (owedLine()): the loop's stack, docs/LISTENING-STATS.md 11.)
__attribute__((noinline)) void ListeningStats::logSaved(sg::Kind k, const sx::SaveReport& rep, float ms) {
  char kind[48];
  if (k == sg::Kind::Rotation) {
    snprintf(kind, sizeof(kind), "rotation, before plays.1.jnl leaves the log");
  } else if (k == sg::Kind::Boot && glue_ && glue_->tries() > 1) {
    snprintf(kind, sizeof(kind), "boot, its try %lu of %lu", static_cast<ul>(glue_->tries()),
             static_cast<ul>(sg::kBootTries));
  } else {
    snprintf(kind, sizeof(kind), "%s", sg::kindName(k));
  }
  switch (rep.what) {
    case sx::Saved::Wrote: {
      char base[96] = "";
      if (rep.compacted) {
        snprintf(base, sizeof(base), "; compacted: stats-%08lx.bin, %lu rows, %lu bytes%s",
                 static_cast<ul>(glue_ ? glue_->fold().head().baseGen : 0), static_cast<ul>(rep.baseRows),
                 static_cast<ul>(rep.baseBytes), rep.oldBaseRemoved ? ", the old base removed" : "");
      } else if (rep.compactFailed) {
        snprintf(base, sizeof(base), "; the compaction FAILED (the base as it was: the next boot's save tries again)");
      } else if (rep.compactSkipped) {
        snprintf(base, sizeof(base), "; the compaction waits for the next boot (its name is the one stats.bin names)");
      }
      Serial.printf("[listen] saved (%s): stats.bin generation %lu, %lu bytes (TRKD %lu rows, RCNT %lu), in %.0f ms%s%s; "
                    "%lu slots folded this session\n",
                    kind, static_cast<ul>(rep.generation), static_cast<ul>(rep.bytes),
                    static_cast<ul>(rep.rows[sf::kTrkd]), static_cast<ul>(rep.rows[sf::kRcnt]), ms,
                    rep.heldPromoted ? "; a held stats.tmp promoted first" : "", base,
                    static_cast<ul>(glue_ ? glue_->fold().folded() : 0));
      return;
    }
    case sx::Saved::Nothing:
      Serial.printf("[listen] saved (%s): nothing new since the card's stats.bin (nothing written%s; %.0f ms)\n", kind,
                    rep.heldPromoted ? "; a held stats.tmp promoted" : "", ms);
      return;
    case sx::Saved::Held:
      Serial.printf("[listen] NOT saved (%s): stats.tmp written whole (%lu bytes) but the card refused its rename: "
                    "held, promoted first by the next save (%.0f ms)\n",
                    kind, static_cast<ul>(rep.bytes), ms);
      return;
    case sx::Saved::Failed:
      Serial.printf("[listen] NOT saved (%s): the card failed its %s step (%.0f ms): the card's stats.bin as it was; "
                    "the trim follows its MARK\n",
                    kind, rep.step, ms);
      return;
    case sx::Saved::Changed:
      Serial.printf("[listen] NOT saved (%s): the card's stats.bin isn't the one this session read or wrote "
                    "(another player's, or deleted on a computer): no save this session, the next boot reads the "
                    "card's\n",
                    kind);
      return;
    case sx::Saved::NoMemory:
      Serial.printf("[listen] NOT saved (%s): no PSRAM for the save's %u B of work memory\n", kind,
                    static_cast<unsigned>(sx::kWorkBytes));
      return;
    case sx::Saved::Off:
      Serial.printf("[listen] NOT saved (%s): %s\n", kind,
                    glue_ && glue_->fold().noMemory() ? "the fold ran out of memory this session"
                                                      : "the stats are off this session");
      return;
    case sx::Saved::Damaged: {
      char sec[16];
      typeText(rep.section, sec, sizeof(sec));
      Serial.printf("[listen] NOT saved (%s): the card's stats.bin is DAMAGED (%s fails its check under the header "
                    "this session stands on; %.0f ms): the session is behind, no save this session; the next boot reads "
                    "it corrupt (stats.bad) and counts again from what the log still holds\n",
                    kind, sec, ms);
      return;
    }
    case sx::Saved::Behind:
      Serial.printf("[listen] NOT saved (%s): %s%s\n", kind,
                    rep.step[0] ? "the stats.tmp a save held was lost before its promotion: " : "",
                    refusedText(sg::Refused::Behind));
      return;
  }
}

// ---------------------------------------------------------------------------
// The lines.
// ---------------------------------------------------------------------------
void ListeningStats::logRead(const sx::Opened& o, const char* when, float ms) {
  char took[32] = "";
  if (ms > 0.0f) snprintf(took, sizeof(took), " in %.0f ms", ms);
  char reads[48] = "";
  if (o.reads > 1 || o.baseReads > 1)
    snprintf(reads, sizeof(reads), " (%lu reads, the base %lu)", static_cast<ul>(o.reads), static_cast<ul>(o.baseReads));
  char twins[64] = "";
  if (o.settled.twins)
    snprintf(twins, sizeof(twins), "; %lu twin%s left (a disk check frees them)", static_cast<ul>(o.settled.twins),
             o.settled.twins == 1 ? "" : "s");
  const char* held = o.heldPromoted ? "; the stats.tmp this session held promoted first" : "";
  switch (o.read) {
    case sx::Read::Loaded: {
      const sf::Head& h = glue_->fold().head();
      char base[160];
      if (o.baseLost) {
        snprintf(base, sizeof(base), "; its base LOST (gone, or another file): the per-track rows before it are "
                                     "gone, the totals stay");
      } else if (h.baseGen) {
        snprintf(base, sizeof(base), "; its base stats-%08lx.bin, %lu rows, %lu bytes, checked whole%s",
                 static_cast<ul>(h.baseGen), static_cast<ul>(h.baseRows), static_cast<ul>(o.baseBytes),
                 o.removed ? " (an old one removed)" : "");
      } else {
        snprintf(base, sizeof(base), "; no base yet");
      }
      char mark[64] = "";
      if (o.markBytes) snprintf(mark, sizeof(mark), " (MARK's block %lu B, kept)", static_cast<ul>(o.markBytes));
      Serial.printf("[listen] stats.bin read %s: generation %lu, %lu bytes%s%s%s%s%s: %llu plays, %lu MARK rows%s; "
                    "counting goes on above MARK\n",
                    when, static_cast<ul>(o.generation), static_cast<ul>(o.bytes), base, took, reads,
                    settledText(o.settled.what), held, static_cast<ull>(h.plays),
                    static_cast<ul>(glue_->fold().marks()), mark);
      break;
    }
    case sx::Read::Absent:
      Serial.printf("[listen] stats.bin read %s: none on the card%s%s%s%s: a fresh start, every slot on the card is "
                    "folded\n",
                    when, took, settledText(o.settled.what), twins, held);
      return;
    case sx::Read::Corrupt:
      Serial.printf("[listen] stats.bin read %s: CORRUPT (%s, read twice%s): %s; counting starts again from the log\n",
                    when, cc::whyName(o.why), took, o.quarantined ? "renamed stats.bad" : "set aside");
      return;
    case sx::Read::Newer:
      Serial.printf("[listen] stats.bin read %s: a NEWER stats firmware's (%s%s): left alone; the stats are OFF this "
                    "session (no fold, no save, no trim)\n",
                    when, cc::whyName(o.why), took);
      return;
    case sx::Read::Unread:
      Serial.printf("[listen] stats.bin read %s: UNREAD (%s%s, %s%s%s): the stats are OFF this session, no fold, no "
                    "save, no trim%s\n",
                    when, o.inBase ? "its base: " : "", cc::whyName(o.why),
                    o.why == cc::Why::FatRecords ? "the file system's records damaged" : "the card failed a read",
                    took, reads, std::strcmp(when, "at the boot") == 0 ? " (tried again at a remount)" : "");
      return;
    case sx::Read::NoMemory:
      Serial.printf("[listen] stats.bin NOT read %s: no PSRAM for the read%s: the stats are OFF this session (no fold, "
                    "no save, no trim)\n",
                    when, o.marks ? ", or for MARK's block (or its rows past the room)" : "");
      return;
  }
  if (o.settled.twins) Serial.printf("[listen] stats.bin's twins:%s\n", twins);
}

// ---------------------------------------------------------------------------
// The console's H.
// ---------------------------------------------------------------------------
void ListeningStats::command(const char* arg) {
  if (!glue_) {
    Serial.println("[listen] no listening stats this boot (no play log)");
    return;
  }
  switch (arg[0]) {
    case 0:
      printState();
      askLook(kLookState, "H: stats.bin on the card (its header and rows)");
      return;
    case 'd': {
      uint16_t today = 0;
      if (!sf::todayOf(boardclock::now(), &today)) {
        Serial.println("[listen] Hd: the board's clock is unset (or not believed): no today, so no last 7 days");
        return;
      }
      askLook(kLookDays, "Hd");
      return;
    }
    case 'm': printMarks(); return;
    case 't': {
      uint64_t h = 0;
      if (!hashOf(arg + 1, &h)) {
        Serial.println("[listen] Ht<16 hex digits>: a track's path hash (METADATA 2.3.3)");
        return;
      }
      lookHash_ = h;
      askLook(kLookTrack, "Ht");
      return;
    }
    case 'r':
    case 'l':
    case 'w':
    case 'p': lists_.command(arg); return;
    case 's':
      if (glue_->booting()) {
        Serial.printf("[listen] Hs refused: %s\n", refusedText(sg::Refused::Booting));
        return;
      }
      hsWaiting_ = true;
      Serial.println("[listen] Hs: a save at the next pass the write gate lets through, after the plays waiting for "
                     "their slots (written first, so the save holds them)");
      return;
    default:
      Serial.println("[listen] H: the state; Hd the last 7 UTC days; Hm MARK, ACKS and the log; Ht<16 hex> a track's "
                     "row; Hs a save now; Hr rank now; Hl<shelf>[window] a list; Hw walk now; Hp the playing track");
      return;
  }
}

void ListeningStats::askLook(uint8_t look, const char* what) {
  looks_ = static_cast<uint8_t>(looks_ | look);
  Serial.printf("[listen] %s: read at the next pass the write gate lets through, after a save due and the plays "
                "waiting for their slots, not behind the library update's fence\n",
                what);
}

void ListeningStats::runLook() {
  if (!glue_ || !looks_) {
    looks_ = 0;
    return;
  }
  // One a pass (one thing of the card's a pass), H's first.
  if (looks_ & kLookState) {
    looks_ = static_cast<uint8_t>(looks_ & ~kLookState);
    printFile();
  } else if (looks_ & kLookDays) {
    looks_ = static_cast<uint8_t>(looks_ & ~kLookDays);
    printDays();
  } else {
    looks_ = 0;
    printTrack(lookHash_);
  }
}

void ListeningStats::printState() {
  statsglue::Glue& g = *glue_;
  const sf::Fold& f = g.fold();
  const sx::Opened& o = g.opened();
  char tries[96] = "";
  if (g.retrying()) {
    const uint64_t now = boardclock::monoMs(), at = g.retryAtMs();
    snprintf(tries, sizeof(tries), "the boot's save tried again in %lu ms (try %lu of %lu), its overflow held",
             static_cast<ul>(at > now ? at - now : 0), static_cast<ul>(g.tries() + 1), static_cast<ul>(sg::kBootTries));
  }
  const char* phase = g.dropped()           ? "OFF (the folds dropped at the end)"
                      : g.rereadPending()   ? "the card mounted again: stats.bin read at the scan's next piece"
                      : !g.on()             ? "OFF"
                      : g.bootPending()     ? "the boot's fold begins at the scan's next piece"
                      : f.booting()         ? "folding (the boot's scan)"
                      : g.retrying()        ? tries
                      : g.partialSeen()     ? "on; a log file not read whole: no save this session"
                      : g.store().changed() ? "on; the card's stats.bin changed: no save this session"
                      : f.noMemory()        ? "on; the fold ran out of memory: no save this session"
                      : f.behind()          ? "on; BEHIND its log: no save this session (the next boot folds what the "
                                              "log still holds; a rotation meanwhile loses those in plays.1.jnl)"
                                            : "on";
  Serial.printf("[listen] H: %s; read %s (%s%s), %lu read again since\n", phase, sx::readName(o.read),
                o.inBase ? "its base: " : "", cc::whyName(o.why), static_cast<ul>(g.rereads()));
  const sf::Head& h = f.head();
  char lis[32], you[32], shuf[32], dj[32];
  listened(h.listenedMs, lis, sizeof(lis));
  listened(h.src[sf::kYou].listenedMs, you, sizeof(you));
  listened(h.src[sf::kShuffle].listenedMs, shuf, sizeof(shuf));
  listened(h.src[sf::kAutoDj].listenedMs, dj, sizeof(dj));
  Serial.printf("[listen] HEAD: %llu plays, %llu skips, %s listened (%llu ms), %llu slots; you %lu/%lu/%s, shuffle "
                "%lu/%lu/%s, Auto DJ %lu/%lu/%s (plays/skips/time); %lu undated; round %lu; twice %lu, gap %lu; flags "
                "%u\n",
                static_cast<ull>(h.plays), static_cast<ull>(h.skips), lis, static_cast<ull>(h.listenedMs),
                static_cast<ull>(h.slots), static_cast<ul>(h.src[sf::kYou].plays), static_cast<ul>(h.src[sf::kYou].skips),
                you, static_cast<ul>(h.src[sf::kShuffle].plays), static_cast<ul>(h.src[sf::kShuffle].skips), shuf,
                static_cast<ul>(h.src[sf::kAutoDj].plays), static_cast<ul>(h.src[sf::kAutoDj].skips), dj,
                static_cast<ul>(h.undated), static_cast<ul>(h.round), static_cast<ul>(h.twice), static_cast<ul>(h.gap),
                static_cast<unsigned>(h.flags));
  uint16_t today = 0;
  const bool known = sf::todayOf(boardclock::now(), &today);
  char todayText[16] = "unknown";
  if (known) dayText(today, todayText, sizeof(todayText));
  if (h.sinceDay == sf::kUnsetDay) {
    Serial.printf("[listen] counting since: not set yet (a save that knows a date sets it); today %s\n", todayText);
  } else {
    char since[16];
    dayText(h.sinceDay, since, sizeof(since));
    const long days = known ? static_cast<long>(today) - static_cast<long>(h.sinceDay) + 1 : 0;
    Serial.printf("[listen] counting since %s (day %u%s); today %s: %ld counting day%s\n", since,
                  static_cast<unsigned>(h.sinceDay),
                  (h.flags & sf::kFlagSinceCommit) ? ", from the sync's commitTime" : "", todayText,
                  days < 1 ? 1L : days, days == 1 ? "" : "s");
  }
  Serial.printf("[listen] RAM: the object %u B, internal (the records %lu of %lu, the save due from %lu; MARK %lu rows "
                "of %lu%s), the overflow %lu chunks (%lu B, %lu B at the peak), the ranges' block %lu B%s; this "
                "session %lu folded, %lu met twice, gap %lu\n",
                static_cast<unsigned>(sizeof(statsglue::Glue)), static_cast<ul>(f.records()),
                static_cast<ul>(sf::kRecords), static_cast<ul>(sf::kSaveAt), static_cast<ul>(f.marks()),
                static_cast<ul>(f.markCap()), g.store().markBytes() ? ", its block in PSRAM" : "",
                static_cast<ul>(f.chunks()), static_cast<ul>(f.held()), static_cast<ul>(f.peak()),
                static_cast<ul>(g.rangesBytes()), g.rangesHeld() ? " (held)" : "", static_cast<ul>(f.folded()),
                static_cast<ul>(f.twice()), static_cast<ul>(f.gap()));
  if (!g.anySave()) {
    Serial.println("[listen] the last save: none asked yet this session");
    return;
  }
  const sx::SaveReport& r = g.last();
  if (g.lastRefused() != sg::Refused::None && r.what != sx::Saved::Damaged && r.what != sx::Saved::Behind) {
    Serial.printf("[listen] the last save: %s, refused (%s); %lu written this session\n", sg::kindName(g.lastKind()),
                  refusedText(g.lastRefused()), static_cast<ul>(g.savesWritten()));
    return;
  }
  if (r.what == sx::Saved::Nothing) {
    // (Nothing written: the report holds no file. The card's copy RAM
    // stands on is the one that holds it all.)
    const sx::Store::Copy& c = g.store().cardCopy();
    char copy[64];
    if (c.present) {
      snprintf(copy, sizeof(copy), "stats.bin generation %lu holds it all", static_cast<ul>(c.generation));
    } else {
      snprintf(copy, sizeof(copy), "no stats.bin on the card");
    }
    Serial.printf("[listen] the last save: %s, nothing new (nothing written; %s), %.0f ms; %lu written this session\n",
                  sg::kindName(g.lastKind()), copy, lastMs_, static_cast<ul>(g.savesWritten()));
    return;
  }
  char sec[24] = "";
  if (r.what == sx::Saved::Damaged) {
    char t[16];
    typeText(r.section, t, sizeof(t));
    snprintf(sec, sizeof(sec), " (%s)", t);
  }
  Serial.printf("[listen] the last save: %s, %s%s%s%s, %lu bytes, generation %lu, %.0f ms%s; %lu written this session\n",
                sg::kindName(g.lastKind()), sx::savedName(r.what), sec, r.step[0] ? " at " : "", r.step,
                static_cast<ul>(r.bytes), static_cast<ul>(r.generation), lastMs_, r.compacted ? ", compacted" : "",
                static_cast<ul>(g.savesWritten()));
}

// H's card part (runLook()): stats.bin's header and directory as the card has
// them now.
void ListeningStats::printFile() {
  statsglue::Glue& g = *glue_;
  const statsglue::Glue::FileLook l = g.lookFile();
  if (l.there && !l.unread) {
    Serial.printf("[listen] stats.bin on the card: generation %lu (the saves), %lu bytes, %s %u.%u; RAM stands on "
                  "%s generation %lu%s\n",
                  static_cast<ul>(l.generation), static_cast<ul>(l.bytes), l.magic == sx::kMagic ? "MPST" : "NOT MPST",
                  static_cast<unsigned>(l.major), static_cast<unsigned>(l.minor),
                  g.store().cardCopy().present ? "its" : "none,", static_cast<ul>(g.store().cardCopy().generation),
                  g.store().held() ? "; a stats.tmp held" : "");
  } else {
    Serial.printf("[listen] stats.bin on the card: %s\n", l.unread ? "unread (the card failed the question)" : "none");
  }
  if (l.frame) {
    const sf::Head& h = g.fold().head();
    Serial.printf("[listen] stats.bin's rows: MARK %lu, HOUR %lu, DAYS %lu, RCNT %lu, TRKD %lu; the base %s%08lx, %lu "
                  "rows%s\n",
                  static_cast<ul>(l.rows[sf::kMark]), static_cast<ul>(l.rows[sf::kHour]),
                  static_cast<ul>(l.rows[sf::kDays]), static_cast<ul>(l.rows[sf::kRcnt]),
                  static_cast<ul>(l.rows[sf::kTrkd]), h.baseGen ? "stats-" : "none ", static_cast<ul>(h.baseGen),
                  static_cast<ul>(h.baseRows), (h.flags & sf::kFlagBaseLost) ? " (a base was lost once)" : "");
  }
}

void ListeningStats::printDays() {
  uint16_t today = 0;
  if (!sf::todayOf(boardclock::now(), &today)) {
    Serial.println("[listen] Hd: the board's clock is unset (or not believed): no today, so no last 7 days");
    return;
  }
  const int64_t t0 = esp_timer_get_time();
  const sg::DaysLook look = glue_->lookDays(today);
  const float ms = msSince(t0);
  uint32_t plays = 0, skips = 0;
  uint64_t total = 0;
  for (uint32_t i = 0; i < 7; ++i) {
    if (today + i < 6) continue;  // (before 2026-01-01)
    const sg::DaysLook::Day& e = look.d[i];
    char date[16];
    dayText(e.day, date, sizeof(date));
    if (!e.any) {
      Serial.printf("[listen] %s (day %u): nothing%s\n", date, static_cast<unsigned>(e.day),
                    e.hours ? " (HOUR has rows!)" : "");
      continue;
    }
    const uint8_t* row = e.row;
    const uint16_t dp = cc::get16(row + 2), ds = cc::get16(row + 4);
    const uint8_t djp = row[6], djs = row[7], shp = row[12], shs = row[13];
    const uint32_t dms = cc::get32(row + 8);
    char lis[32];
    listened(dms, lis, sizeof(lis));
    Serial.printf("[listen] %s (day %u): %u plays, %u skips, %s (%lu ms); you %u/%u, shuffle %u/%u, Auto DJ %u/%u; "
                  "HOUR: %lu plays in %lu hours, %llu ms\n",
                  date, static_cast<unsigned>(e.day), static_cast<unsigned>(dp), static_cast<unsigned>(ds), lis,
                  static_cast<ul>(dms), static_cast<unsigned>(dp - djp - shp), static_cast<unsigned>(ds - djs - shs),
                  static_cast<unsigned>(shp), static_cast<unsigned>(shs), static_cast<unsigned>(djp),
                  static_cast<unsigned>(djs), static_cast<ul>(e.hourPlays), static_cast<ul>(e.hours),
                  static_cast<ull>(e.hourMs));
    plays += dp;
    skips += ds;
    total += dms;
  }
  char lis[32];
  listened(total, lis, sizeof(lis));
  Serial.printf("[listen] the last 7 UTC days: %lu plays, %lu skips, %s listened (stats.bin's DAYS and HOUR with the "
                "%lu records%s; %.0f ms)\n",
                static_cast<ul>(plays), static_cast<ul>(skips), lis, static_cast<ul>(glue_->fold().records()),
                look.read ? "" : "; stats.bin NOT read: the records alone", ms);
}

void ListeningStats::printMarks() {
  statsglue::Glue& g = *glue_;
  const sf::Fold& f = g.fold();
  const cc::msmf::Acks& a = g.acks();
  char dev[16];
  // MARK's rows, each beside ACKS's row and the log's highest seq.
  for (uint32_t i = 0; i < f.marks(); ++i) {
    const sf::Fold::MarkRow* m = f.markRow(i);
    if (!m) break;
    devText(m->dev, dev, sizeof(dev));
    char ack[24] = "none", log[40] = "none";
    for (uint32_t k = 0; k < a.count; ++k)
      if (a.entries[k].epoch == m->epoch() && std::memcmp(a.entries[k].deviceId, m->dev, 6) == 0)
        snprintf(ack, sizeof(ack), "%lu", static_cast<ul>(a.entries[k].seq));
    for (uint32_t k = 0; k < g.logKeys(); ++k) {
      const sg::LogKey& lk = g.logKey(k);
      if (lk.epoch == m->epoch() && std::memcmp(lk.dev, m->dev, 6) == 0)
        snprintf(log, sizeof(log), "%lu (%lu slots)", static_cast<ul>(lk.seq), static_cast<ul>(lk.slots));
    }
    Serial.printf("[listen] MARK %s epoch %016llx: %lu (the card's %lu%s); ACKS %s; the log %s\n", dev,
                  static_cast<ull>(m->epoch()), static_cast<ul>(m->seq), static_cast<ul>(m->card),
                  f.heldPending() ? ", a held stats.tmp's at its promotion" : "", ack, log);
  }
  // ACKS's rows and the log's keys with no MARK row.
  for (uint32_t k = 0; k < a.count; ++k) {
    if (f.markOf(a.entries[k].deviceId, a.entries[k].epoch)) continue;
    devText(a.entries[k].deviceId, dev, sizeof(dev));
    Serial.printf("[listen] ACKS %s epoch %016llx: %lu; no MARK row\n", dev, static_cast<ull>(a.entries[k].epoch),
                  static_cast<ul>(a.entries[k].seq));
  }
  for (uint32_t k = 0; k < g.logKeys(); ++k) {
    const sg::LogKey& lk = g.logKey(k);
    if (f.markOf(lk.dev, lk.epoch)) continue;
    devText(lk.dev, dev, sizeof(dev));
    Serial.printf("[listen] the log %s epoch %016llx: %lu (%lu slots); no MARK row\n", dev, static_cast<ull>(lk.epoch),
                  static_cast<ul>(lk.seq), static_cast<ul>(lk.slots));
  }
  Serial.printf("[listen] Hm: MARK %lu rows (room for %lu), ACKS %lu (%lu on the card), the log's %lu keys%s (the boot's "
                "scan); HEAD's twice %lu, gap %lu; this boot %lu met twice, gap %lu\n",
                static_cast<ul>(f.marks()), static_cast<ul>(f.markCap()), static_cast<ul>(a.count),
                static_cast<ul>(a.rows), static_cast<ul>(g.logKeys()), g.logKeysMore() ? " and more" : "",
                static_cast<ul>(f.head().twice), static_cast<ul>(f.head().gap), static_cast<ul>(f.twice()),
                static_cast<ul>(f.gap()));
}

void ListeningStats::printTrack(uint64_t h) {
  const int64_t t0 = esp_timer_get_time();
  const sg::TrackLook t = glue_->track(h);
  printTrackLook("track", h, t, msSince(t0));
}

void ListeningStats::printTrackLook(const char* who, uint64_t h, const sg::TrackLook& t, float ms) {
  const char* base = t.base == sg::TrackLook::Base::None       ? "no base"
                     : t.base == sg::TrackLook::Base::Found    ? "in the base"
                     : t.base == sg::TrackLook::Base::NotThere ? "not in the base"
                                                               : "the base UNREAD";
  const char* delta = t.unread ? ", stats.bin's TRKD UNREAD" : t.delta ? ", and in TRKD or the records" : "";
  if (t.row.hash == 0) {
    Serial.printf("[listen] %s %016llx: no row (%s%s; %lu base reads, %.1f ms)\n", who, static_cast<ull>(h), base,
                  t.unread ? delta : ", not in TRKD or the records", static_cast<ul>(t.reads), ms);
    return;
  }
  char first[24], last[24];
  timeText(t.row.firstAt, first, sizeof(first));
  timeText(t.row.lastAt, last, sizeof(last));
  Serial.printf("[listen] %s %016llx: %u plays, %u skips; first %s (%s), last %s; lastOrd %lu (%s%s; %lu base reads, "
                "%.1f ms)\n",
                who, static_cast<ull>(h), static_cast<unsigned>(t.row.plays), static_cast<unsigned>(t.row.skips), first,
                srcName(t.row.src), last, static_cast<ul>(t.row.lastOrd), base, delta, static_cast<ul>(t.reads), ms);
}
