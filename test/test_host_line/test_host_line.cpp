// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for HostLine (docs/USB-VISUALIZER.md "Framing"): the
// computer's '@' lines on the serial console, which must never reach the
// console's single-key switch (where 'f' forgets the headphones and
// restarts), and the tokenizer and number parsers behind them.
// Run: pio test -e native
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "HostLine.h"

using B = HostLine::Byte;

namespace {
// A reader past its start-up Sync (as after the boot's first quiet moment).
HostLine synced() {
  HostLine h;
  char key = 0;
  TEST_ASSERT_FALSE(h.quiet(HostLine::kQuietMs, &key));
  TEST_ASSERT_FALSE(h.syncing());
  return h;
}

// Pushes `s` (not atIsText) as one read at `nowMs` and returns what each byte was.
std::vector<B> pushAll(HostLine& h, const std::string& s, bool atIsText = false, uint32_t nowMs = 0) {
  std::vector<B> out;
  for (char c : s) out.push_back(h.push(c, atIsText, nowMs));
  return out;
}

// What the sender writes (tools/usb_viz.py --dry-run): a hello with a hex
// session id ('f' forgets the headphones, 'a' injects a touch), an epoch,
// hops with floats ('e', '-', '.'), clocks, the beat log, a bye.
const char* const kSenderLines[] = {
    "@hello 1 fa0d3e1b viz\n",
    "@e 1 44100 0\n",
    "@h 1 15 2.27439774e-08 0.0425188579\n",
    "@h 1 16 0.681935132 1.05966449\n",
    "@c 1 52368 1\n",
    "@h 1 18 0.00732411351 5.22125702e-05\n",
    "@log 2\n",
    "@c 12 -4410 0\r\n",
    "@bye\n",
};

std::string senderStream() {
  std::string s;
  for (const char* l : kSenderLines) s += l;
  return s;
}

std::string lineOf(HostLine& h, const std::string& s) {
  const std::vector<B> r = pushAll(h, s);
  TEST_ASSERT_TRUE(r.back() == B::Line);
  return h.text();
}
}  // namespace

void setUp() {}
void tearDown() {}

// '@' starts a line, '\n' or '\r' ends it; the bytes between are the line's.
void test_a_line_from_at_to_the_end() {
  HostLine h = synced();
  std::vector<B> r = pushAll(h, "x@hello 1 a viz\n");
  TEST_ASSERT_TRUE(r[0] == B::Console);
  TEST_ASSERT_TRUE(r[1] == B::Start);
  for (size_t i = 2; i + 1 < r.size(); ++i) TEST_ASSERT_TRUE(r[i] == B::Taken);
  TEST_ASSERT_TRUE(r.back() == B::Line);
  TEST_ASSERT_EQUAL_STRING("@hello 1 a viz", h.text());
  TEST_ASSERT_FALSE(h.inLine());

  TEST_ASSERT_EQUAL_STRING("@bye", lineOf(h, "@bye\r").c_str());
  // "\r\n": the line, then a lone '\n' for the console (which ignores it).
  r = pushAll(h, "@c 1 2 1\r\n");
  TEST_ASSERT_TRUE(r[r.size() - 2] == B::Line);
  TEST_ASSERT_TRUE(r.back() == B::Console);
  TEST_ASSERT_EQUAL_STRING("@c 1 2 1", h.text());
  // Just "@": a line, which the tokenizer refuses.
  TEST_ASSERT_EQUAL_STRING("@", lineOf(h, "@\n").c_str());
  HostFields f;
  char text[8] = "@";
  TEST_ASSERT_FALSE(splitHostLine(text, &f));
}

// Every console key inside a line belongs to the line: 'f' (forget the
// headphones and restart), ' ' (play), '+', digits, Enter only ends it.
void test_console_keys_inside_a_line_are_the_lines() {
  HostLine h = synced();
  const std::string keys = "@fnp +-szdmxXvLiIbBcqaPTRuwgejktyh0123456789";
  const std::vector<B> r = pushAll(h, keys);
  for (size_t i = 1; i < r.size(); ++i) TEST_ASSERT_TRUE(r[i] == B::Taken);
  TEST_ASSERT_TRUE(h.inLine());
  // No timeout: still the line's however long the writer pauses (nothing
  // here reads a clock), until the terminator.
  TEST_ASSERT_TRUE(h.push('f', false, 0) == B::Taken);
  TEST_ASSERT_TRUE(h.push('\n', false, 0) == B::Line);
  TEST_ASSERT_TRUE(h.push('f', false, 0) == B::Console);  // after it: the console's again
}

// 255 bytes from the '@' pass; the 256th makes the line Long, and the rest
// is swallowed up to the terminator, never the console's.
void test_overlong_lines() {
  HostLine h = synced();
  std::string ok = "@" + std::string(HostLine::kMaxLine - 1, 'a');
  TEST_ASSERT_EQUAL_UINT32(255, ok.size());
  TEST_ASSERT_EQUAL_STRING(ok.c_str(), lineOf(h, ok + "\n").c_str());

  std::string longer = "@" + std::string(HostLine::kMaxLine, 'b') + "fff";
  const std::vector<B> r = pushAll(h, longer + "\n");
  for (size_t i = 1; i + 1 < r.size(); ++i) TEST_ASSERT_TRUE(r[i] == B::Taken);
  TEST_ASSERT_TRUE(r.back() == B::Long);
  TEST_ASSERT_TRUE(h.push('f', false, 0) == B::Console);
  // A long line of junk bytes is Long (the length wins).
  std::string junk = "@" + std::string(300, '\x01');
  TEST_ASSERT_TRUE(pushAll(h, junk + "\r").back() == B::Long);
}

// A byte outside 0x20-0x7E (a control character, a tab, 0x80 and up)
// makes the line Bad; it is still consumed up to its end.
void test_bad_bytes() {
  for (char c : {'\x01', '\t', '\x1b', '\x7f', static_cast<char>(0x80), static_cast<char>(0xff), '\0'}) {
    HostLine h = synced();
    std::string s = "@h 1 2 ";
    s += c;
    s += "f 3\n";
    const std::vector<B> r = pushAll(h, s);
    for (size_t i = 1; i + 1 < r.size(); ++i) TEST_ASSERT_TRUE(r[i] == B::Taken);
    TEST_ASSERT_TRUE(r.back() == B::Bad);
    TEST_ASSERT_EQUAL_STRING("@ok", lineOf(h, "@ok\n").c_str());  // the next line is fine
  }
}

// '@' inside a line drops the partial line (Restart) and starts a new one:
// a sender that died mid-line can't swallow the next session's @hello.
void test_at_mid_line_restarts() {
  HostLine h = synced();
  std::vector<B> r = pushAll(h, "@h 3 120 0.12");
  r = pushAll(h, "@hello 1 b viz\n");
  TEST_ASSERT_TRUE(r[0] == B::Restart);
  TEST_ASSERT_TRUE(r.back() == B::Line);
  TEST_ASSERT_EQUAL_STRING("@hello 1 b viz", h.text());
  // After an overlong or bad start too.
  pushAll(h, "@" + std::string(400, 'x'));
  pushAll(h, "\x02");
  TEST_ASSERT_EQUAL_STRING("@bye", lineOf(h, "@bye\n").c_str());
}

// Where the console says '@' is text (an R argument that has text:
// "Rttone:1000@48000"), '@' is the console's; anywhere else it starts a line.
void test_at_as_text() {
  HostLine h = synced();
  TEST_ASSERT_TRUE(h.push('@', true, 0) == B::Console);
  TEST_ASSERT_FALSE(h.inLine());
  TEST_ASSERT_TRUE(h.push('@', false, 0) == B::Start);
  TEST_ASSERT_TRUE(h.inLine());
  // Inside a line the flag doesn't matter: still the line's.
  TEST_ASSERT_TRUE(h.push('x', true, 0) == B::Taken);
  TEST_ASSERT_TRUE(h.push('@', true, 0) == B::Restart);
  TEST_ASSERT_TRUE(h.push('\n', true, 0) == B::Line);
}

// 10,000 random streams of console bytes and host lines (good, bad,
// overlong, cut off by an '@', ended by '\n', '\r' or "\r\n"): every byte
// from an '@' to its terminator is the line's, never Console; every other
// byte is the console's; each line ends as what it is.
void test_fuzz_no_host_byte_reaches_the_console() {
  std::mt19937 rng(20261001);
  auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<uint32_t>(n)); };
  const std::string consoleKeys = "fnp +-szdmxXvLibcthykqaPTIBRuwgej0123456789\n\r";
  int lines = 0, bads = 0, longs = 0, restarts = 0;
  for (int stream = 0; stream < 10000; ++stream) {
    HostLine h = synced();
    struct Expect {
      char c;
      bool host;
      B end;  // for a terminator: what the line is
    };
    std::vector<Expect> bytes;
    const int segments = 1 + pick(12);
    for (int s = 0; s < segments; ++s) {
      if (pick(3) == 0) {  // console bytes (never '@': that would start a line)
        for (int k = pick(6); k > 0; --k) bytes.push_back({consoleKeys[pick(static_cast<int>(consoleKeys.size()))], false, B::Console});
        continue;
      }
      // A host line, maybe with a partial line cut off by '@' first.
      int pieces = pick(4) == 0 ? 2 : 1;
      for (int piece = 0; piece < pieces; ++piece) {
        bytes.push_back({'@', true, B::Console});
        const int kind = pick(10);
        const int len = kind == 0 ? 255 + pick(200) : pick(60);
        bool bad = false;
        for (int k = 0; k < len; ++k) {
          char c;
          if (kind == 1 && pick(20) == 0) {
            do {
              c = static_cast<char>(pick(256));
            } while (c == '\n' || c == '\r' || c == '@');
          } else {
            c = static_cast<char>(0x20 + pick(0x5F));
            if (c == '@') c = 'f';
          }
          const auto u = static_cast<unsigned char>(c);
          if (u < 0x20 || u > 0x7E) bad = true;
          bytes.push_back({c, true, B::Console});
        }
        if (piece + 1 < pieces) continue;  // cut off: the next '@' restarts
        const bool tooLong = 1 + len > static_cast<int>(HostLine::kMaxLine);
        const B end = tooLong ? B::Long : bad ? B::Bad : B::Line;
        const int term = pick(3);
        bytes.push_back({term == 1 ? '\r' : '\n', true, end});
        if (term == 2) {
          bytes.back().c = '\r';
          bytes.push_back({'\n', false, B::Console});  // "\r\n": the '\n' is the console's
        }
      }
    }
    for (const Expect& e : bytes) {
      const B got = h.push(e.c, false, 0);
      if (!e.host) {
        TEST_ASSERT_TRUE_MESSAGE(got == B::Console, "a console byte was taken");
        continue;
      }
      TEST_ASSERT_TRUE_MESSAGE(got != B::Console, "a host byte reached the console");
      if (e.c == '\n' || e.c == '\r') {
        TEST_ASSERT_TRUE_MESSAGE(got == e.end, "a line ended as something else");
        lines += got == B::Line;
        bads += got == B::Bad;
        longs += got == B::Long;
      }
      restarts += got == B::Restart;
    }
  }
  char msg[96];
  snprintf(msg, sizeof(msg), "%d lines, %d bad, %d long, %d cut off", lines, bads, longs, restarts);
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE(lines > 1000 && bads > 100 && longs > 100 && restarts > 100);
}

// ---- Sync: reading that starts partway through a line ----

// A boot (or a lost stretch of input) can start the reader at any byte of
// the sender's stream. From every offset, read in one go or a byte at a
// time (1 ms apart, quiet() asked between bytes as the console does):
// nothing is the console's, no key comes out of quiet(), and every whole
// line after the cut is still a Line.
void test_sync_no_tail_reaches_the_console() {
  const std::string stream = senderStream();
  for (size_t from = 0; from < stream.size(); ++from) {
    const std::string tail = stream.substr(from);
    uint32_t wholeLines = 0;  // the lines that start at or after `from`
    size_t at = 0;
    for (const char* l : kSenderLines) {
      if (at >= from) ++wholeLines;
      at += strlen(l);
    }
    for (int bytewise = 0; bytewise < 2; ++bytewise) {
      HostLine h;
      uint32_t now = 1000, lines = 0;
      char key = 0;
      for (char c : tail) {
        const B got = h.push(c, false, now);
        TEST_ASSERT_TRUE_MESSAGE(got != B::Console, "a tail byte reached the console");
        if (got == B::Line) ++lines;
        if (bytewise) {
          ++now;
          TEST_ASSERT_FALSE_MESSAGE(h.quiet(now, &key), "a tail byte came out of quiet()");
        }
      }
      TEST_ASSERT_EQUAL_UINT32(wholeLines, lines);
      TEST_ASSERT_FALSE(h.quiet(now + HostLine::kQuietMs, &key));  // nothing held: no key
      TEST_ASSERT_FALSE(h.syncing());
      TEST_ASSERT_TRUE(h.push('s', false, now + 100) == B::Console);  // the console's from now on
    }
  }
}

// In Sync a lone byte followed by quiet is a keypress: quiet() hands it
// over (and Sync ends). Nothing at all for kQuietMs ends Sync too. A key
// sent with its Enter in one write is dropped (as a tail would be).
void test_sync_lets_a_lone_keypress_through() {
  HostLine h;
  char key = 0;
  TEST_ASSERT_TRUE(h.syncing());
  TEST_ASSERT_TRUE(h.push('s', false, 500) == B::Held);
  TEST_ASSERT_FALSE(h.quiet(500 + HostLine::kQuietMs - 1, &key));
  TEST_ASSERT_TRUE(h.syncing());
  TEST_ASSERT_TRUE(h.quiet(500 + HostLine::kQuietMs, &key));
  TEST_ASSERT_EQUAL_CHAR('s', key);
  TEST_ASSERT_FALSE(h.syncing());
  TEST_ASSERT_TRUE(h.push('n', false, 600) == B::Console);

  HostLine quietBoot;  // nothing arrived since Serial began (SerialConsole::begin())
  TEST_ASSERT_FALSE(quietBoot.quiet(HostLine::kQuietMs, &key));
  TEST_ASSERT_FALSE(quietBoot.syncing());
  TEST_ASSERT_TRUE(quietBoot.push('f', false, 30) == B::Console);

  HostLine withEnter;
  const std::vector<B> r = pushAll(withEnter, "i3\n", false, 10);
  TEST_ASSERT_TRUE(r[0] == B::Held);
  TEST_ASSERT_TRUE(r[1] == B::Dropped && r[2] == B::Dropped);
  TEST_ASSERT_EQUAL_UINT32(2, withEnter.dropped());
  TEST_ASSERT_FALSE(withEnter.quiet(10 + HostLine::kQuietMs, &key));
  TEST_ASSERT_FALSE(withEnter.syncing());
  TEST_ASSERT_TRUE(pushAll(withEnter, "i3\n", false, 50)[0] == B::Console);  // typed again: a key

  HostLine thenLine;  // a held byte, then a line: the byte is dropped, the line read
  TEST_ASSERT_TRUE(thenLine.push('z', false, 10) == B::Held);
  TEST_ASSERT_EQUAL_STRING("@bye", lineOf(thenLine, "@bye\n").c_str());
  TEST_ASSERT_EQUAL_UINT32(1, thenLine.dropped());
  TEST_ASSERT_FALSE(thenLine.quiet(1000, &key));
}

// A tail, or a line, that pauses for longer than kQuietMs still runs to its
// terminator: the quiet doesn't end Sync in the middle of it.
void test_sync_waits_for_a_terminator() {
  HostLine h;
  char key = 0;
  pushAll(h, "3 0.0425", false, 10);  // a tail
  TEST_ASSERT_FALSE(h.quiet(1000, &key));
  TEST_ASSERT_TRUE(h.syncing());
  TEST_ASSERT_TRUE(h.push('f', false, 1000) == B::Dropped);  // still the tail's
  TEST_ASSERT_TRUE(h.push('\n', false, 1000) == B::Dropped);
  pushAll(h, "@c 1 44", false, 1001);  // a line under way
  TEST_ASSERT_FALSE(h.quiet(2000, &key));
  TEST_ASSERT_TRUE(h.syncing());
  TEST_ASSERT_TRUE(pushAll(h, "10 1\n", false, 2000).back() == B::Line);
  TEST_ASSERT_EQUAL_STRING("@c 1 4410 1", h.text());
  TEST_ASSERT_FALSE(h.quiet(2000 + HostLine::kQuietMs, &key));
  TEST_ASSERT_FALSE(h.syncing());
}

// resync() (input lost: a receive overflow): a line under way ends Bad,
// whatever tail follows the loss is dropped, the next line is read, and a
// quiet moment gives the console its keys back.
void test_resync_after_lost_input() {
  HostLine h = synced();
  char key = 0;
  pushAll(h, "@h 1 7 0.1", false, 100);
  h.resync(200);
  TEST_ASSERT_TRUE(h.syncing());
  TEST_ASSERT_TRUE(pushAll(h, "2 0.3\n", false, 200).back() == B::Bad);  // straddled the loss
  // Lost right after a terminator: the next line's tail.
  for (B b : pushAll(h, "a0d3e1b viz\n", false, 200)) TEST_ASSERT_TRUE(b == B::Dropped || b == B::Held);
  TEST_ASSERT_EQUAL_UINT32(11, h.dropped());
  TEST_ASSERT_EQUAL_STRING("@c 1 2 1", lineOf(h, "@c 1 2 1\n").c_str());
  TEST_ASSERT_FALSE(h.quiet(200 + HostLine::kQuietMs, &key));
  TEST_ASSERT_TRUE(h.push('f', false, 300) == B::Console);
  // A held key is dropped by a resync.
  h.resync(400);
  TEST_ASSERT_TRUE(h.push('s', false, 400) == B::Held);
  h.resync(401);
  TEST_ASSERT_FALSE(h.quiet(500, &key));
}

// 2,000 sender streams cut at a random byte and read in random chunks,
// with gaps between lines now and then long enough to end Sync: no byte
// is ever the console's (but a "\r\n"'s lone '\n' after Sync, which it
// ignores) and no key comes out of quiet().
void test_fuzz_sync_cut_streams() {
  std::mt19937 rng(20261002);
  auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<uint32_t>(n)); };
  const int nLines = static_cast<int>(sizeof(kSenderLines) / sizeof(kSenderLines[0]));
  for (int stream = 0; stream < 2000; ++stream) {
    std::vector<std::string> lines;
    for (int k = 2 + pick(20); k > 0; --k) lines.push_back(kSenderLines[pick(nLines)]);
    lines[0] = lines[0].substr(static_cast<size_t>(pick(static_cast<int>(lines[0].size()))));
    HostLine h;
    uint32_t now = static_cast<uint32_t>(pick(5000));
    char key = 0;
    for (const std::string& l : lines) {
      size_t i = 0;
      while (i < l.size()) {
        const size_t n = 1 + static_cast<size_t>(pick(static_cast<int>(l.size() - i)));
        for (size_t k = 0; k < n; ++k) {
          // (Once Sync is over, "\r\n"'s lone '\n' is the console's, which ignores it.)
          const char c = l[i + k];
          const B got = h.push(c, false, now);
          TEST_ASSERT_TRUE_MESSAGE(got != B::Console || (c == '\n' && !h.syncing()), "a byte reached the console");
        }
        i += n;
        now += static_cast<uint32_t>(pick(3));  // inside a line: back to back
        TEST_ASSERT_FALSE_MESSAGE(h.quiet(now, &key), "a key came out of quiet()");
      }
      now += static_cast<uint32_t>(pick(4) == 0 ? static_cast<int>(HostLine::kQuietMs) + pick(100) : pick(5));
      TEST_ASSERT_FALSE_MESSAGE(h.quiet(now, &key), "a key came out of quiet()");
    }
  }
}

// ---- the tokenizer ----

void test_split_into_verb_and_fields() {
  HostFields f;
  char a[] = "@hello 1 7f3a viz";
  TEST_ASSERT_TRUE(splitHostLine(a, &f));
  TEST_ASSERT_EQUAL_STRING("hello", f.verb);
  TEST_ASSERT_EQUAL_INT(3, f.count);
  TEST_ASSERT_EQUAL_STRING("1", f.field[0]);
  TEST_ASSERT_EQUAL_STRING("7f3a", f.field[1]);
  TEST_ASSERT_EQUAL_STRING("viz", f.field[2]);
  TEST_ASSERT_FALSE(f.more);

  char b[] = "@h  12   4310 0.1 0.2   ";  // more spaces, trailing ones
  TEST_ASSERT_TRUE(splitHostLine(b, &f));
  TEST_ASSERT_EQUAL_STRING("h", f.verb);
  TEST_ASSERT_EQUAL_INT(4, f.count);
  TEST_ASSERT_EQUAL_STRING("0.2", f.field[3]);

  char c[] = "@bye";
  TEST_ASSERT_TRUE(splitHostLine(c, &f));
  TEST_ASSERT_EQUAL_STRING("bye", f.verb);
  TEST_ASSERT_EQUAL_INT(0, f.count);

  char d[] = "@wifi.set c3NpZA cGFzcw";
  TEST_ASSERT_TRUE(splitHostLine(d, &f));
  TEST_ASSERT_EQUAL_STRING("wifi.set", f.verb);

  char e[] = "@x 1 2 3 4 5 6 7 8 9 10";  // more than 8: the first 8, the rest ignored
  TEST_ASSERT_TRUE(splitHostLine(e, &f));
  TEST_ASSERT_EQUAL_INT(8, f.count);
  TEST_ASSERT_TRUE(f.more);
  TEST_ASSERT_EQUAL_STRING("8", f.field[7]);

  char g[] = "@abcdefghijklmnop 1";  // a 16-byte verb
  TEST_ASSERT_TRUE(splitHostLine(g, &f));
  char k[] = "@log2 1";
  TEST_ASSERT_TRUE(splitHostLine(k, &f));
}

void test_split_refuses_bad_verbs() {
  const char* bad[] = {"@", "@ hello", "@Hello 1", "@1abc", "@he_llo", "@wifi..set", "@a.", "@.a", "@a.1b",
                       "@abcdefghijklmnopq 1", "hello 1", "@-", "@h\x01"};
  for (const char* s : bad) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", s);
    HostFields f;
    TEST_ASSERT_FALSE_MESSAGE(splitHostLine(buf, &f), s);
    TEST_ASSERT_EQUAL_STRING("", f.verb);
  }
  HostFields f;
  TEST_ASSERT_FALSE(splitHostLine(nullptr, &f));
}

// ---- the numbers ----

void test_u32() {
  uint32_t v = 7;
  TEST_ASSERT_TRUE(parseHostU32("0", &v));
  TEST_ASSERT_EQUAL_UINT32(0, v);
  TEST_ASSERT_TRUE(parseHostU32("4294967295", &v));
  TEST_ASSERT_EQUAL_UINT32(4294967295u, v);
  TEST_ASSERT_TRUE(parseHostU32("0044100", &v));
  TEST_ASSERT_EQUAL_UINT32(44100, v);
  for (const char* s : {"4294967296", "99999999999999999999", "-1", "+1", "", " 1", "1 ", "1a", "0x10", "1.0", "1e3"}) {
    v = 7;
    TEST_ASSERT_FALSE_MESSAGE(parseHostU32(s, &v), s);
    TEST_ASSERT_EQUAL_UINT32(7, v);  // untouched
  }
  TEST_ASSERT_FALSE(parseHostU32(nullptr, &v));
}

void test_i32() {
  int32_t v = 7;
  TEST_ASSERT_TRUE(parseHostI32("-6615", &v));
  TEST_ASSERT_EQUAL_INT32(-6615, v);
  TEST_ASSERT_TRUE(parseHostI32("2147483647", &v));
  TEST_ASSERT_EQUAL_INT32(2147483647, v);
  TEST_ASSERT_TRUE(parseHostI32("-2147483648", &v));
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, v);
  TEST_ASSERT_TRUE(parseHostI32("-0", &v));
  TEST_ASSERT_EQUAL_INT32(0, v);
  for (const char* s : {"2147483648", "-2147483649", "-", "--1", "+5", "", "1-", "1.5", "0x7f", "12345678901234"}) {
    TEST_ASSERT_FALSE_MESSAGE(parseHostI32(s, &v), s);
  }
}

void test_f32() {
  float v = 7.0f;
  TEST_ASSERT_TRUE(parseHostF32("0", &v));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v);
  TEST_ASSERT_TRUE(parseHostF32("0.0291153", &v));
  TEST_ASSERT_EQUAL_FLOAT(0.0291153f, v);
  TEST_ASSERT_TRUE(parseHostF32("1.49884555e-15", &v));
  TEST_ASSERT_EQUAL_FLOAT(1.49884555e-15f, v);
  TEST_ASSERT_TRUE(parseHostF32("1E4", &v));
  TEST_ASSERT_EQUAL_FLOAT(1e4f, v);
  TEST_ASSERT_TRUE(parseHostF32("2.5e+1", &v));
  TEST_ASSERT_EQUAL_FLOAT(25.0f, v);
  TEST_ASSERT_TRUE(parseHostF32("-1.5", &v));
  TEST_ASSERT_EQUAL_FLOAT(-1.5f, v);
  TEST_ASSERT_TRUE(parseHostF32("120.", &v));
  TEST_ASSERT_EQUAL_FLOAT(120.0f, v);
  TEST_ASSERT_TRUE(parseHostF32("1e-50", &v));  // below a float's range: 0, finite
  TEST_ASSERT_TRUE(v >= 0.0f && v < 1e-37f);
  // The exact text a float prints as round-trips.
  char text[32];
  snprintf(text, sizeof(text), "%.9g", 0.00515992753f);
  TEST_ASSERT_TRUE(parseHostF32(text, &v));
  TEST_ASSERT_TRUE(v == 0.00515992753f);
  for (const char* s : {".5", "nan", "NAN", "inf", "-inf", "infinity", "0x10", "0x1p3", "1e", "1e+", "e5", "1e39",
                        "-1e39", "", "-", "+1", " 1", "1 ", "1,5", "1.2.3", "1e5.5", "1f"}) {
    v = 7.0f;
    TEST_ASSERT_FALSE_MESSAGE(parseHostF32(s, &v), s);
    TEST_ASSERT_EQUAL_FLOAT(7.0f, v);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_line_from_at_to_the_end);
  RUN_TEST(test_console_keys_inside_a_line_are_the_lines);
  RUN_TEST(test_overlong_lines);
  RUN_TEST(test_bad_bytes);
  RUN_TEST(test_at_mid_line_restarts);
  RUN_TEST(test_at_as_text);
  RUN_TEST(test_fuzz_no_host_byte_reaches_the_console);
  RUN_TEST(test_sync_no_tail_reaches_the_console);
  RUN_TEST(test_sync_lets_a_lone_keypress_through);
  RUN_TEST(test_sync_waits_for_a_terminator);
  RUN_TEST(test_resync_after_lost_input);
  RUN_TEST(test_fuzz_sync_cut_streams);
  RUN_TEST(test_split_into_verb_and_fields);
  RUN_TEST(test_split_refuses_bad_verbs);
  RUN_TEST(test_u32);
  RUN_TEST(test_i32);
  RUN_TEST(test_f32);
  return UNITY_END();
}
