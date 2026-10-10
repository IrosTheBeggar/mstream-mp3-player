// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "HostLine.h"
#include "HostStatus.h"

// The USB visualizer's session on the Core2 (docs/USB-VISUALIZER.md
// "Sessions", "The visualizer's messages", "Errors"): what each of the
// computer's lines means, in the style of IdlePolicy: each call returns an
// Out (an event for the firmware to carry out, and a reply line to send),
// and nothing here reads a clock or touches the hardware (host-tested:
// test_host_link; app/UsbViz carries it out).
//
//   @hello <proto> <session> <features>   starts host mode (Enter) when the
//       features hold "viz" and nothing refuses it (Busy: @err 4); again
//       with the same id: @ok again, nothing else; with another id: the
//       sender started over (Restart: the epoch forgotten, host mode kept).
//   @bye                                  ends it (Exit, "@bye ok").
//   @e <epoch> <rate> <prior>             a new epoch (Epoch: the tracker
//       reset at that rate and prior); the same epoch and rate again with
//       another prior: Prior; another rate within an epoch: @err 5.
//   @h <epoch> <hop> <low> <mid>          a hop's energies (Hop). The first
//       after @e, and any hop after a gap in the numbers, comes with
//       `restart` (the tracker starts over at that hop); a hop number seen
//       already is a duplicate (dropped, counted).
//   @c <epoch> <heard> <playing>          the heard clock (Clock).
//   @log <0|1|2>                          the per-beat log (Log).
// Lines for another epoch than the current one are stale: dropped, counted.
//
// The computer's questions about the board (docs/HOST-STATUS.md), in a
// session or not, while declined too (they drive nothing on the Core2):
//   @status                               Status (app/HostQuery answers it).
//   @count                                Count (... starts one, or refuses).
//   @identify <label>                     Identify, with the label (1 to 16
//       bytes: hoststatus::validLabel()); none: @err 1, longer: @err 5.
// They change nothing here: not a session's timeout, nor its counters, nor
// a decline's quiet (a player that asks for @status every second can start
// the dancer again once its own user asks).
//
// Any valid line keeps the session alive; poll() ends it after kTimeoutMs
// without one (@bye timeout) or when USB power goes (Unplugged: no reply,
// no link). end() is the Core2's side: a touch, a button, the headphones'
// play key (@bye user, then everything the computer sends is declined,
// @err 8, until it has been quiet for kDeclineQuietMs: the user wins), or
// the Dance tab gone some other way (@bye dance). Refusals are @err lines,
// at most kMaxErrorsPerSecond (more are only counted); an error never
// changes anything.
struct HostStats {
  uint32_t lines = 0;       // valid lines
  uint32_t epochs = 0;
  uint32_t hops = 0;        // fed to the tracker
  uint32_t gaps = 0;        // hop numbers skipped (the tracker restarted)
  uint32_t dup = 0;         // hop numbers seen already
  uint32_t stale = 0;       // lines for another epoch
  uint32_t clocks = 0;
  uint32_t bad = 0;         // malformed: a bad byte, overlong, cut off by an '@', a syntax error
  uint32_t errors = 0;      // refused lines (@err), sent or not
  uint32_t suppressed = 0;  // ... of which not sent (the rate limit)
};

class HostLink {
public:
  static constexpr uint32_t kProtoMin = 1, kProtoMax = 1;
  static constexpr uint32_t kTimeoutMs = 3000;
  static constexpr uint32_t kDeclineQuietMs = 3000;
  static constexpr uint32_t kMaxErrorsPerSecond = 4;
  static constexpr size_t kMaxSession = 16;
  static constexpr float kMaxEnergy = 1e4f;
  static constexpr const char* kCaps = "viz,log";

  enum class Event : uint8_t { None, Enter, Restart, Exit, Epoch, Prior, Hop, Clock, Log, Status, Count, Identify };
  enum class Why : uint8_t { Bye, Timeout, Unplugged, Touch, Button, HeadsetKey, DanceGone };
  // Why an @hello can't start host mode now (the first that applies).
  enum class Busy : uint8_t { None, Ui, Screen, Pairing, Dance };
  enum Err : uint8_t {
    kSyntax = 1, kVersion = 2, kNoSession = 3, kBusy = 4, kRange = 5, kLong = 6, kVerb = 7, kDeclined = 8, kNoEpoch = 9,
  };

  struct Out {
    Event event = Event::None;
    Why why = Why::Bye;            // Exit
    uint32_t epoch = 0, rate = 0;  // Epoch, Prior, Hop, Clock
    float prior = 0.0f;            // Epoch, Prior
    uint32_t hop = 0;              // Hop
    bool restart = false;          // Hop: reset the tracker at this hop first
    bool gap = false;              // Hop: ... because of a gap in the numbers
    float low = 0.0f, mid = 0.0f;  // Hop
    int32_t heard = 0;             // Clock
    bool playing = false;          // Clock
    uint8_t level = 0;             // Log
    char label[hoststatus::kMaxLabel + 1] = "";  // Identify
    char reply[96] = "";           // a line to send back (without its '\n'), or ""
  };

  // The firmware version for @ok (spaces become '_'; cut to 48 bytes).
  void begin(const char* fw);
  // A complete line from the console (HostLine::Line): cut up in place.
  Out line(char* text, uint32_t nowMs, Busy busy);
  // A line that wasn't one: Bad, Long, or Restart (cut off by an '@': no reply).
  Out bad(HostLine::Byte kind, uint32_t nowMs);
  // Every loop pass: the timeout, USB power gone, the end of a decline.
  Out poll(uint32_t nowMs, bool usbPower);
  // The Core2 ends it: Touch, Button, HeadsetKey (declines after), DanceGone,
  // Unplugged. Nothing when not active.
  Out end(Why why, uint32_t nowMs);
  // A line refused for a reason only the firmware knows (a @count while
  // playing: "@err 4 count playing"): its @err, within the rate limit and
  // counted as any other. Changes nothing else.
  Out refuse(uint32_t nowMs, uint8_t code, const char* verb, const char* detail = nullptr) {
    return error(nowMs, code, verb, detail);
  }

  bool active() const { return active_; }
  bool declined() const { return declined_; }
  uint32_t proto() const { return proto_; }
  const char* session() const { return session_; }
  bool haveEpoch() const { return haveEpoch_; }
  uint32_t epoch() const { return epoch_; }
  uint32_t rate() const { return rate_; }
  float prior() const { return prior_; }
  uint8_t logLevel() const { return log_; }
  uint32_t enteredMs() const { return enteredMs_; }
  // Since the session began (reset at Enter).
  const HostStats& stats() const { return stats_; }

  static const char* whyName(Why why);   // "the computer said bye", "a touch", ...
  static const char* busyName(Busy b);   // "ui", "screen", "pairing", "dance"

private:
  Out error(uint32_t nowMs, uint8_t code, const char* verb, const char* detail = nullptr);
  Out hello(const HostFields& f, uint32_t nowMs, Busy busy);
  Out epochLine(const HostFields& f, uint32_t nowMs);
  Out hopLine(const HostFields& f, uint32_t nowMs);
  Out clockLine(const HostFields& f, uint32_t nowMs);
  Out logLine(const HostFields& f, uint32_t nowMs);
  Out identifyLine(const HostFields& f, uint32_t nowMs);
  void heardFrom(uint32_t nowMs);  // any line: the decline's quiet starts again
  void valid(uint32_t nowMs);      // a valid line in a session: alive

  char fw_[49] = "?";
  bool active_ = false;
  uint32_t proto_ = 0;
  char session_[kMaxSession + 1] = "";
  uint32_t enteredMs_ = 0;
  uint32_t lastValidMs_ = 0;
  bool declined_ = false;
  uint32_t lastLineMs_ = 0;   // any line at all (the decline's quiet)
  bool haveEpoch_ = false;
  uint32_t epoch_ = 0, rate_ = 0;
  float prior_ = 0.0f;
  bool haveHop_ = false;      // a hop fed since the epoch began
  uint32_t nextHop_ = 0;
  uint8_t log_ = 0;
  uint32_t errWindowMs_ = 0;  // the rate limit's second
  uint32_t errInWindow_ = 0;
  HostStats stats_;
};
