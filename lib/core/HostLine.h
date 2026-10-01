// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// The computer's lines on the serial console (docs/USB-VISUALIZER.md
// "Framing"): '@' to '\n' or '\r', never a console key. The console reads
// every byte through push() before its single-key switch:
//
//   - outside a line, '@' starts one (Start: a console command waiting for
//     its argument is abandoned), except where the console says '@' is text
//     (`atIsText`: an R argument that has text, "Rttone:1000@48000"); any
//     other byte is the console's (Console);
//   - inside a line every byte is the line's (Taken), whatever it is, until
//     '\n' or '\r' ends it: Line (complete: text()), Bad (a byte outside
//     0x20-0x7E was in it) or Long (more than kMaxLine bytes from the '@';
//     the rest swallowed up to the end). There is no timeout inside a line:
//     a slow writer's tail can't be read as console keys;
//   - an '@' inside a line drops the partial line (Restart: counted as bad,
//     no reply) and starts a new one there, so a sender that died mid-line
//     can't swallow the next session's @hello.
//
// So "\r\n" gives a line and then a lone '\n' for the console, which ignores
// it (no command can be waiting: the '@' abandoned it).
//
// All of that needs the '@'. Reading that starts partway through a line
// (Serial starting while a computer sends: a boot; or input lost to a
// receive overflow) would hand the line's tail to the keys ("viz" ends in
// 'i' 'z', a session id can hold an 'f'). So the reader starts in Sync, and
// resync() puts it back there:
//   - '@' starts a line as ever (Sync carries on after it);
//   - any other byte outside a line is Held, not the console's yet. If more
//     bytes follow it (in the same read, or before quiet() saw kQuietMs of
//     nothing), it was a line's tail: it and the rest up to the terminator
//     are Dropped. If the input goes quiet after it, it was a keypress:
//     quiet() hands it to the console. (A key sent with its Enter in one
//     write is dropped too: the first key after a boot may need typing again.)
//   - quiet() ends Sync once nothing arrived for kQuietMs outside a line or
//     a tail: no line can be under way then, and every byte after is read
//     as above. A tail or a line under way waits for its terminator first.
// Portable, no allocation: the buffer is kMaxLine + 1 bytes in the object.
class HostLine {
public:
  static constexpr size_t kMaxLine = 255;  // from the '@' to the last byte before the terminator
  // A line's bytes come back to back (87 us apiece at 115200 baud, a USB
  // packet's worth at a time): this long with nothing is a gap between
  // lines, never one inside a line.
  static constexpr uint32_t kQuietMs = 20;
  enum class Byte : uint8_t {
    Console, Start, Restart, Taken, Line, Bad, Long,
    Held,     // Sync: a byte outside a line, kept until quiet() or the next byte says what it was
    Dropped,  // Sync: part of a line's tail (or a lone terminator), for no one
  };

  // `nowMs`: when the byte was read (the bytes of one read share it).
  Byte push(char c, bool atIsText, uint32_t nowMs);
  // Nothing more to read now. In Sync, after kQuietMs with nothing, outside
  // a line or a tail: Sync ends, and true when the byte it held was a
  // keypress (`*key`: the console's now). Otherwise false.
  bool quiet(uint32_t nowMs, char* key);
  // Back to Sync: input was lost. A line under way ends Bad (its bytes may
  // straddle the loss), a held byte is dropped.
  void resync(uint32_t nowMs);
  bool syncing() const { return sync_; }
  // Bytes dropped as a line's tail (terminators not counted) since the
  // last resync() (or construction).
  uint32_t dropped() const { return dropped_; }
  // After Line: the line from its '@', without the terminator, 0-terminated
  // (writable: splitHostLine() cuts it up in place). Valid until the next push().
  char* text() { return buf_; }
  bool inLine() const { return inLine_; }

private:
  Byte startLine();

  char buf_[kMaxLine + 1] = "";
  size_t len_ = 0;
  bool inLine_ = false;
  bool bad_ = false;
  bool long_ = false;
  bool sync_ = true;     // from construction: the boot's first byte may be mid-line
  bool tail_ = false;    // Sync: dropping a line's tail up to its terminator
  char held_ = 0;        // Sync: the byte held (0: none)
  uint32_t lastMs_ = 0;  // the last byte read (0: none since Serial began, at boot)
  uint32_t dropped_ = 0;
};

// A host line cut into its verb and fields (pointers into the line).
struct HostFields {
  static constexpr int kMaxFields = 8;
  static constexpr size_t kMaxVerb = 16;
  const char* verb = "";
  const char* field[kMaxFields] = {};
  int count = 0;      // fields after the verb (at most kMaxFields)
  bool more = false;  // there were more: ignored, as any extra trailing field
};

// Splits `line` ("@verb f1 f2 ...") in place: the verb ([a-z][a-z0-9]*,
// optionally dotted: "wifi.set"; at most 16 bytes) and up to 8 fields,
// separated by one or more spaces (leading and trailing spaces ignored).
// False: a syntax error (no '@', no verb, a bad verb); `out->verb` is then "".
bool splitHostLine(char* line, HostFields* out);

// The protocol's numbers (docs/USB-VISUALIZER.md "Fields"), strictly: no
// spaces, no '+', no hex, no nan or inf. False: not one, or out of range.
bool parseHostU32(const char* s, uint32_t* out);  // [0-9]+, at most 4294967295
bool parseHostI32(const char* s, int32_t* out);   // -?[0-9]+
bool parseHostF32(const char* s, float* out);     // -?[0-9]+(\.[0-9]*)?([eE][-+]?[0-9]+)?, finite as a float
