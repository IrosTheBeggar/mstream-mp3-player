// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "HostLink.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
// Milliseconds from `thenMs` to `nowMs`; 0 when `thenMs` is the later one.
// The loop reads its clock once a pass, before the console stamps the
// pass's lines with millis(), so poll() can see a line a few ms "after" its
// own now: unsigned, that was ~49 days, and every session timed out (and
// every decline ended) in the pass it began.
uint32_t since(uint32_t nowMs, uint32_t thenMs) {
  const auto d = static_cast<int32_t>(nowMs - thenMs);
  return d > 0 ? static_cast<uint32_t>(d) : 0;
}

bool sameVerb(const HostFields& f, const char* v) { return std::strcmp(f.verb, v) == 0; }

// 1-16 of [0-9A-Za-z].
bool validSession(const char* s) {
  const size_t n = std::strlen(s);
  if (n == 0 || n > HostLink::kMaxSession) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
  }
  return true;
}

// A comma list of [a-z]+. False: malformed. `viz`: it names "viz".
bool parseFeatures(const char* s, bool* viz) {
  *viz = false;
  const char* item = s;
  for (const char* p = s;; ++p) {
    if (*p == ',' || *p == '\0') {
      const size_t n = static_cast<size_t>(p - item);
      if (n == 0) return false;
      if (n == 3 && std::strncmp(item, "viz", 3) == 0) *viz = true;
      if (*p == '\0') return true;
      item = p + 1;
      continue;
    }
    if (*p < 'a' || *p > 'z') return false;
  }
}

bool energyInRange(float e) { return e >= 0.0f && e <= HostLink::kMaxEnergy; }
}  // namespace

void HostLink::begin(const char* fw) {
  size_t n = 0;
  for (; fw && fw[n] && n < sizeof(fw_) - 1; ++n) {
    const auto c = static_cast<unsigned char>(fw[n]);
    fw_[n] = c > 0x20 && c < 0x7F ? static_cast<char>(c) : '_';
  }
  fw_[n] = '\0';
  if (n == 0) std::snprintf(fw_, sizeof(fw_), "?");
}

const char* HostLink::whyName(Why why) {
  switch (why) {
    case Why::Bye: return "the computer said bye";
    case Why::Timeout: return "nothing from the computer for 3 s";
    case Why::Unplugged: return "USB unplugged";
    case Why::Touch: return "a touch";
    case Why::Button: return "a button";
    case Why::HeadsetKey: return "the headphones' play key";
    case Why::DanceGone: return "the Dance tab closed";
  }
  return "?";
}

const char* HostLink::busyName(Busy b) {
  switch (b) {
    case Busy::None: return "-";
    case Busy::Ui: return "ui";
    case Busy::Screen: return "screen";
    case Busy::Pairing: return "pairing";
    case Busy::Dance: return "dance";
  }
  return "?";
}

void HostLink::heardFrom(uint32_t nowMs) {
  // A decline ends once the computer has been quiet for kDeclineQuietMs:
  // measured up to this line, which then counts as the next one.
  if (declined_ && since(nowMs, lastLineMs_) >= kDeclineQuietMs) declined_ = false;
  lastLineMs_ = nowMs;
}

void HostLink::valid(uint32_t nowMs) {
  lastValidMs_ = nowMs;
  ++stats_.lines;
}

HostLink::Out HostLink::error(uint32_t nowMs, uint8_t code, const char* verb, const char* detail) {
  Out o;
  ++stats_.errors;
  if (code == kSyntax || code == kLong) ++stats_.bad;
  if (nowMs - errWindowMs_ >= 1000) {  // (line times only: they never run backwards)
    errWindowMs_ = nowMs;
    errInWindow_ = 0;
  }
  if (errInWindow_ >= kMaxErrorsPerSecond) {
    ++stats_.suppressed;
    return o;
  }
  ++errInWindow_;
  if (detail) {
    std::snprintf(o.reply, sizeof(o.reply), "@err %u %s %s", static_cast<unsigned>(code), verb, detail);
  } else {
    std::snprintf(o.reply, sizeof(o.reply), "@err %u %s", static_cast<unsigned>(code), verb);
  }
  return o;
}

HostLink::Out HostLink::bad(HostLine::Byte kind, uint32_t nowMs) {
  heardFrom(nowMs);
  if (kind == HostLine::Byte::Restart) {  // cut off by the next '@': counted, no reply
    ++stats_.bad;
    return Out{};
  }
  return error(nowMs, kind == HostLine::Byte::Long ? kLong : kSyntax, "-");
}

HostLink::Out HostLink::line(char* text, uint32_t nowMs, Busy busy) {
  HostFields f;
  const bool split = splitHostLine(text, &f);
  // The board's questions (docs/HOST-STATUS.md): in a session or not,
  // declined or not; the firmware carries them out. Not the dancer's
  // lines: a decline's quiet goes on through them (a player that asks for
  // @status every second can start the dancer again once its user asks).
  if (split && (sameVerb(f, "status") || sameVerb(f, "count"))) {
    Out o;
    o.event = sameVerb(f, "status") ? Event::Status : Event::Count;
    return o;
  }
  if (split && sameVerb(f, "identify")) return identifyLine(f, nowMs);
  heardFrom(nowMs);
  if (!split) return error(nowMs, kSyntax, "-");
  if (sameVerb(f, "hello")) return hello(f, nowMs, busy);
  if (sameVerb(f, "bye")) {
    if (!active_) return Out{};  // outside a session: ignored
    valid(nowMs);
    Out o = end(Why::Bye, nowMs);
    return o;
  }
  const bool data = sameVerb(f, "e") || sameVerb(f, "h") || sameVerb(f, "c") || sameVerb(f, "log");
  if (!data) return error(nowMs, kVerb, f.verb);  // (s, t, wifi.*, pair.*, ...: not in this firmware)
  if (declined_) return error(nowMs, kDeclined, f.verb);
  if (!active_) return error(nowMs, kNoSession, f.verb);
  if (sameVerb(f, "e")) return epochLine(f, nowMs);
  if (sameVerb(f, "h")) return hopLine(f, nowMs);
  if (sameVerb(f, "c")) return clockLine(f, nowMs);
  return logLine(f, nowMs);
}

HostLink::Out HostLink::hello(const HostFields& f, uint32_t nowMs, Busy busy) {
  uint32_t proto = 0;
  bool viz = false;
  if (f.count < 3 || !parseHostU32(f.field[0], &proto) || !validSession(f.field[1]) ||
      !parseFeatures(f.field[2], &viz)) {
    return error(nowMs, kSyntax, "hello");
  }
  if (declined_) return error(nowMs, kDeclined, "hello");
  if (proto < kProtoMin) {
    char range[24];
    std::snprintf(range, sizeof(range), "%u-%u", static_cast<unsigned>(kProtoMin), static_cast<unsigned>(kProtoMax));
    return error(nowMs, kVersion, "hello", range);
  }
  if (!viz) return error(nowMs, kVerb, "hello");  // no feature this firmware knows
  Out o;
  if (active_) {
    if (std::strcmp(session_, f.field[1]) != 0) {
      // The sender started over (it restarted, or saw a reboot): host mode
      // stays, the epoch goes.
      o.event = Event::Restart;
      std::snprintf(session_, sizeof(session_), "%s", f.field[1]);
      haveEpoch_ = false;
      haveHop_ = false;
    }
  } else {
    if (busy != Busy::None) return error(nowMs, kBusy, "hello", busyName(busy));
    o.event = Event::Enter;
    active_ = true;
    std::snprintf(session_, sizeof(session_), "%s", f.field[1]);
    enteredMs_ = nowMs;
    haveEpoch_ = false;
    haveHop_ = false;
    log_ = 0;
    stats_ = HostStats{};
  }
  proto_ = proto < kProtoMax ? proto : kProtoMax;
  valid(nowMs);
  std::snprintf(o.reply, sizeof(o.reply), "@ok %u %s %s %s", static_cast<unsigned>(proto_), session_, fw_, kCaps);
  return o;
}

HostLink::Out HostLink::epochLine(const HostFields& f, uint32_t nowMs) {
  uint32_t epoch = 0, rate = 0;
  float prior = 0.0f;
  if (f.count < 3 || !parseHostU32(f.field[0], &epoch) || !parseHostU32(f.field[1], &rate) ||
      !parseHostF32(f.field[2], &prior)) {
    return error(nowMs, kSyntax, "e");
  }
  if ((rate != 44100 && rate != 48000) || (prior != 0.0f && (prior < 30.0f || prior > 300.0f))) {
    return error(nowMs, kRange, "e");
  }
  Out o;
  if (haveEpoch_ && epoch == epoch_) {
    if (rate != rate_) return error(nowMs, kRange, "e");  // the rate can't change within an epoch
    valid(nowMs);
    if (prior != prior_) {
      prior_ = prior;
      o.event = Event::Prior;
      o.epoch = epoch_;
      o.rate = rate_;
      o.prior = prior_;
    }
    return o;
  }
  valid(nowMs);
  haveEpoch_ = true;
  epoch_ = epoch;
  rate_ = rate;
  prior_ = prior;
  haveHop_ = false;
  ++stats_.epochs;
  o.event = Event::Epoch;
  o.epoch = epoch;
  o.rate = rate;
  o.prior = prior;
  return o;
}

HostLink::Out HostLink::hopLine(const HostFields& f, uint32_t nowMs) {
  uint32_t epoch = 0, hop = 0;
  float low = 0.0f, mid = 0.0f;
  if (f.count < 4 || !parseHostU32(f.field[0], &epoch) || !parseHostU32(f.field[1], &hop) ||
      !parseHostF32(f.field[2], &low) || !parseHostF32(f.field[3], &mid)) {
    return error(nowMs, kSyntax, "h");
  }
  if (!energyInRange(low) || !energyInRange(mid)) return error(nowMs, kRange, "h");
  if (!haveEpoch_) return error(nowMs, kNoEpoch, "h");
  valid(nowMs);
  Out o;
  if (epoch != epoch_) {
    ++stats_.stale;
    return o;
  }
  if (haveHop_ && hop != nextHop_) {
    // (hop - nextHop_ as a signed difference: a number below the next one
    // is one seen already. An epoch is far shorter than 2^31 hops.)
    if (static_cast<int32_t>(hop - nextHop_) < 0) {
      ++stats_.dup;
      return o;
    }
    ++stats_.gaps;
    o.restart = true;
    o.gap = true;
  }
  if (!haveHop_) o.restart = true;
  haveHop_ = true;
  nextHop_ = hop + 1;
  ++stats_.hops;
  o.event = Event::Hop;
  o.epoch = epoch_;
  o.rate = rate_;
  o.hop = hop;
  o.low = low;
  o.mid = mid;
  return o;
}

HostLink::Out HostLink::clockLine(const HostFields& f, uint32_t nowMs) {
  uint32_t epoch = 0, playing = 0;
  int32_t heard = 0;
  if (f.count < 3 || !parseHostU32(f.field[0], &epoch) || !parseHostI32(f.field[1], &heard) ||
      !parseHostU32(f.field[2], &playing)) {
    return error(nowMs, kSyntax, "c");
  }
  if (playing > 1) return error(nowMs, kRange, "c");
  if (!haveEpoch_) return error(nowMs, kNoEpoch, "c");
  valid(nowMs);
  Out o;
  if (epoch != epoch_) {
    ++stats_.stale;
    return o;
  }
  ++stats_.clocks;
  o.event = Event::Clock;
  o.epoch = epoch_;
  o.rate = rate_;
  o.heard = heard;
  o.playing = playing == 1;
  return o;
}

HostLink::Out HostLink::logLine(const HostFields& f, uint32_t nowMs) {
  uint32_t level = 0;
  if (f.count < 1 || !parseHostU32(f.field[0], &level)) return error(nowMs, kSyntax, "log");
  if (level > 2) return error(nowMs, kRange, "log");
  valid(nowMs);
  Out o;
  log_ = static_cast<uint8_t>(level);
  o.event = Event::Log;
  o.level = log_;
  return o;
}

HostLink::Out HostLink::identifyLine(const HostFields& f, uint32_t nowMs) {
  if (f.count < 1) return error(nowMs, kSyntax, "identify");
  if (!hoststatus::validLabel(f.field[0])) return error(nowMs, kRange, "identify");  // over 16 bytes
  Out o;
  o.event = Event::Identify;
  std::snprintf(o.label, sizeof(o.label), "%s", f.field[0]);
  return o;
}

HostLink::Out HostLink::poll(uint32_t nowMs, bool usbPower) {
  if (declined_ && since(nowMs, lastLineMs_) >= kDeclineQuietMs) declined_ = false;
  if (!active_) return Out{};
  if (!usbPower) return end(Why::Unplugged, nowMs);
  if (since(nowMs, lastValidMs_) >= kTimeoutMs) return end(Why::Timeout, nowMs);
  return Out{};
}

HostLink::Out HostLink::end(Why why, uint32_t nowMs) {
  Out o;
  if (!active_) return o;
  active_ = false;
  haveEpoch_ = false;
  haveHop_ = false;
  log_ = 0;
  o.event = Event::Exit;
  o.why = why;
  switch (why) {
    case Why::Bye:
      std::snprintf(o.reply, sizeof(o.reply), "@bye ok");
      break;
    case Why::Timeout:
      std::snprintf(o.reply, sizeof(o.reply), "@bye timeout");
      break;
    case Why::Touch:
    case Why::Button:
    case Why::HeadsetKey:
      // The user wins: the computer is declined until it has been quiet.
      std::snprintf(o.reply, sizeof(o.reply), "@bye user");
      declined_ = true;
      lastLineMs_ = nowMs;
      break;
    case Why::DanceGone:
      std::snprintf(o.reply, sizeof(o.reply), "@bye dance");
      break;
    case Why::Unplugged:
      break;  // no link to say it on
  }
  return o;
}
