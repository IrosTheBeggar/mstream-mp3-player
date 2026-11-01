// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// The computer's questions about the board (docs/HOST-STATUS.md), the parts
// with no hardware in them: the @status line's text, the @identify label's
// rule, the FSINFO rule that says when FatFs's free count is a count, and
// when the @count's progress lines go out. The firmware fills the facts
// from what it already holds (app/HostQuery, main.cpp's hook); nothing here
// reads the card or a clock. Host-tested: test_host_status.
//
//   @status fw=v0.9.0 elf=63ee7a2b card=fat32 size=63864569856 free=38214565888 tracks=1284 music=? bat=87
//           state=playing bt=Paul%27s%20headphones
//
// (one line). The fields in this order, each key=value, one space between;
// a value never holds a space ('?' unknown, '-' none). A reader ignores
// keys it doesn't know: a field added later goes in before bt=, which
// stays last (it is the one that gives way when the line runs long).
namespace hoststatus {

// A reply's bytes from its '@', as a host line's (HostLine::kMaxLine): the
// computer reads the replies with the same limit.
constexpr size_t kMaxLine = 255;
// fw= is cut to this many bytes (as @ok's version: HostLink::begin()).
constexpr size_t kMaxFw = 48;
// bt= cut short ends with this, "…" (U+2026) percent-encoded.
constexpr const char* kEllipsis = "%E2%80%A6";

// card=: what is in the slot. Fat32 and Fat16 are mounted (FAT12, a card of
// a few MB that FatFs mounts too, says fat16); the rest didn't mount, as
// cardformat::Kind names them: None (nothing answered: no card), ExFat,
// Ntfs, Gpt, Other (a card answered, nothing recognised: blank, Linux's, a
// FAT that didn't mount) and Unreadable (a card answered and its first
// sector couldn't be read).
enum class Card : uint8_t { None, Fat32, Fat16, ExFat, Ntfs, Gpt, Other, Unreadable };
// state=: the player's (Waiting, a play waiting for the headphones, is
// playing); host while a computer drives the dancer (docs/USB-VISUALIZER.md).
enum class State : uint8_t { Idle, Paused, Playing, Host };
// tracks=: the library index's count, building while the library update's
// build has it (its fence: docs/METADATA.md 3.4.2), '-' with no card.
enum class Tracks : uint8_t { None, Building, Count };

struct Facts {
  const char* fw = "";   // the app description's version (app/Version: appDesc())
  const char* elf = "";  // the ELF's SHA-256, its first 8 hex digits (About's)
  Card card = Card::None;
  uint64_t sizeBytes = 0;  // the card's (its CSD's sector count); 0: '-'
  bool counting = false;   // a @count is under way: free=counting
  bool freeKnown = false;  // freeBytes is FatFs's free count (FSINFO's when valid, or a count's); else '?'
  uint64_t freeBytes = 0;
  Tracks tracks = Tracks::None;
  uint32_t trackCount = 0;
  bool musicKnown = false;  // the indexed tracks' bytes (this firmware's index doesn't keep them: '?')
  uint64_t musicBytes = 0;
  int battery = -1;  // 0-100 (more reads 100); negative: '-'
  State state = State::Idle;
  const char* bt = nullptr;  // the paired headphones' name (UTF-8); null or "": '-'
};

// The @status line into `out` (no '\n'; at most kMaxLine bytes and its
// terminator, cut to `size`): its length.
size_t format(const Facts& f, char* out, size_t size);

const char* cardName(Card c);  // "none", "fat32", ... "unreadable"
const char* stateName(State s);  // "idle", "paused", "playing", "host"

// RFC 3986's unreserved bytes (A-Z a-z 0-9 - . _ ~) as they are, every
// other byte as %XX (upper-case hex): a value with no space and nothing
// outside 0x21-0x7E, which a URL decoder reads back to the same UTF-8
// (Paul's headphones: Paul%27s%20headphones). Whole characters only, while
// they fit in `room` bytes: a UTF-8 sequence (a lead byte and its
// continuation bytes) is never split. `out` holds room + 1 bytes (the
// terminator). Returns the bytes written; `*taken`, if given, the input
// bytes encoded (len when it all fit).
size_t percentEncode(const char* in, size_t len, char* out, size_t room, size_t* taken = nullptr);

// @identify's label (the computer's name for the port: COM5, ttyACM0): 1
// to kMaxLabel bytes of 0x21-0x7E (a host line's field can't hold more
// than that anyway). The banner draws it (test_ui_library: the widest fit).
constexpr size_t kMaxLabel = 16;
bool validLabel(const char* s);

// The FSINFO rule. FatFs's free count (FATFS::free_clst) is a count only
// when it is at most the volume's clusters (n_fatent - 2), FatFs's own test
// (f_getfree(), create_chain()). The mount takes it from FAT32's FSINFO
// sector as it is: "unknown" there is 0xFFFFFFFF, which FatFs also sets on
// FAT12/16 and when the FSINFO sector is missing or broken; any other value
// above the clusters is a broken FSINFO, unknown as well. Never zero or
// full for unknown.
constexpr bool freeCountValid(uint32_t freeClusters, uint32_t fatEntries) {
  return fatEntries >= 3 && freeClusters <= fatEntries - 2;
}

// When @count's "@count <percent>" lines go out: "@count 0" as it starts
// (start()), then one each time the percent reaches another tenth, and
// one at least every kQuietMs while the percent moves (a tenth of a big
// card's FAT can take a while), the last at 100, just before "@count done".
class Progress {
public:
  static constexpr uint8_t kStep = 10;
  static constexpr uint32_t kQuietMs = 5000;
  // The count began: its "@count 0" goes out now.
  void start(uint32_t nowMs) {
    last_ = 0;
    lastMs_ = nowMs;
  }
  // The percent now (0-100): true when a line is due, which is then the
  // last sent.
  bool due(uint8_t percent, uint32_t nowMs);
  uint8_t last() const { return last_; }

private:
  uint8_t last_ = 0;
  uint32_t lastMs_ = 0;
};

}  // namespace hoststatus
