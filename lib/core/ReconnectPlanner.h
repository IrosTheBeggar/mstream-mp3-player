// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// How the Core2 looks for its headphones while no link is up: when to page
// the remembered ones, when to scan for them by name, and when to stop and
// only stay connectable. Portable, so the schedule can be tested on the
// host; BtSink's PlayerA2dp feeds it the stack's state and carries out what
// step() returns (docs/ENERGY.md item 1).
//
// Measured on the device: the old cycle (the library's pages, then a minute
// of inquiry, forever) cost +35.5 mA (USB) for as long as the headphones
// were away, 32% of the whole streaming draw, on a device playing nothing;
// an idle Core2 that is only connectable (page scan) costs ~nothing. And the
// inquiry can't find remembered headphones unless they are in pairing mode.
// So:
//   - Burst: on a drop, at boot and on any listener's ask (BtSink::connect():
//     a play waiting for them, Connect or a tap on the card, a B hold, the
//     console's o; the Pair screen closing without a pairing; the console's
//     Pr1), kBurstPages pages, the next once the last one has ended and
//     kBurstGapMs after it began. A page already on its way when the burst
//     starts is its first. Opening the Output tab is not an ask: the
//     resting card would never be seen (its Connect is one tap away).
//   - Backoff: then one page at a time, kBackoffMs apart (30 s, 1, 2, 5
//     min, then every 5 min), never an inquiry.
//   - Resting: after kGiveUpMs without success, or at once after the burst
//     while nobody is around (the screen off, nothing playing or waiting:
//     In::quiet), no pages and no scans. The Core2 stays connectable, so
//     headphones that are switched on or taken out of their case come back
//     by themselves (the user's do). A listener's ask starts a burst again.
//   - Scan: with nothing remembered, a scan by name (inquiry) for
//     kScanForMs after the boot or an ask, then Resting, only when
//     sinksearch::mayScan() allows one (In::canScan: a developer build's
//     BT_SINK_NAME, or the console's Bs). A release build has no name: with
//     nothing remembered it goes straight to Resting, at the boot and on
//     every ask, and never scans (pairing is the Pair screen's). Never
//     while headphones are remembered: a scan running then is stopped.
//
// The library's own auto-reconnect is kept disarmed outside a pairing (its
// "retries exhausted: start discovery" branch must never run): every
// background page comes from here.
class ReconnectPlanner {
public:
  enum class Lib : uint8_t {
    Unconnected,  // the library's UNCONNECTED (CONNECTED without a link counts as this)
    Connecting,   // a connection under way (the boot's page, a device a scan found)
    Discovering,  // scanning
    Other,        // DISCOVERED (connecting to a device found) and the rest
  };

  enum class Phase : uint8_t {
    Idle,     // not looking: linked, the listener let go, a pairing, the Pair screen, before the stack is up
    Burst,    // paging, kBurstPages times
    Backoff,  // a page now and then, no inquiry
    Resting,  // connectable only: no pages, no scans
    Scan,     // nothing remembered, a name to look for: a scan by name
  };

  // Why a burst starts (for the log).
  enum class Why : uint8_t { Boot, Drop, Ask };

  struct In {
    bool linked;           // an A2DP link is up
    Lib state;
    bool discoveryActive;  // the stack reports a scan running
    bool remembered;       // a device address is remembered
    bool canScan;          // a scan may run (sinksearch::mayScan() with nothing remembered: a name, or Bs)
    bool quiet;            // nobody is around: the screen is off and nothing plays or waits
    uint32_t nowMs;
  };

  enum class Do : uint8_t {
    Nothing,
    Page,      // page the remembered headphones (connect_to: pageMade() follows)
    Scan,      // start a scan by name (the library's DISCOVERING; it runs rounds until stopped)
    StopScan,  // stop the scan running (state UNCONNECTED, then cancel)
  };

  static constexpr int kBurstPages = 3;
  static constexpr uint32_t kBurstGapMs = 10000;  // a burst's pages begin at least this far apart (~ the heartbeat)
  // A page this recent may still be answered: the controller's page timeout
  // (5.12 s by default) and a margin. Nothing pages on top of it.
  static constexpr uint32_t kPageMs = 5500;
  static constexpr int kBackoffSteps = 4;
  static constexpr uint32_t kBackoffMs[kBackoffSteps] = {30000, 60000, 120000, 300000};  // then the last, again
  static constexpr uint32_t kGiveUpMs = 15u * 60u * 1000u;  // from the burst's start: then Resting
  static constexpr uint32_t kScanForMs = 120000;            // a scan by name, from the boot or the ask

  // A burst from now; with nothing remembered, a scan by name if
  // `canScan` (as In::canScan), else Resting.
  void start(Why why, bool remembered, bool canScan, uint32_t nowMs);
  // Every page made, ours or the library's (the boot's, a pairing's).
  void pageMade(uint32_t nowMs);
  // The page got its answer without a link (refused, timed out).
  void pageEnded(uint32_t nowMs);
  // A link came up, or the listener took over (let go, the Pair screen, a
  // pairing): nothing to plan until the next start().
  void linked() {
    phase_ = Phase::Idle;
    pageOpen_ = false;  // answered
  }
  void stop() { phase_ = Phase::Idle; }
  // Resting now (the console's Pr0, a power measurement): a scan by name
  // running is stopped by the next step().
  void rest() { phase_ = Phase::Resting; }

  // On BtAppT's ticks (a few times a second) and heartbeats while no link is up.
  Do step(const In& in);

  Phase phase() const { return phase_; }
  // In a burst: the number of the last page made (1..kBurstPages); else 0.
  int burstTry() const { return phase_ == Phase::Burst ? pages_ : 0; }
  // A page made within kPageMs that hasn't had its answer.
  bool pageOnItsWay(uint32_t nowMs) const;
  // Backoff: how long until the next page (for the log); 0 otherwise.
  uint32_t nextPageInMs(uint32_t nowMs) const;
  static const char* phaseName(Phase p);
  static const char* whyName(Why w);

private:
  bool elapsed(uint32_t nowMs, uint32_t sinceMs, uint32_t ms) const {
    return static_cast<int32_t>(nowMs - sinceMs) >= static_cast<int32_t>(ms);
  }
  uint32_t backoffMs() const { return kBackoffMs[step_ < kBackoffSteps ? step_ : kBackoffSteps - 1]; }

  Phase phase_ = Phase::Idle;
  int pages_ = 0;           // Burst: pages made in it
  int step_ = 0;            // Backoff: the next gap's index into kBackoffMs
  uint32_t sinceMs_ = 0;    // the burst's (or the scan's) start
  bool paged_ = false;      // a page was made (lastPageMs_: when)
  bool pageOpen_ = false;   // ... and hasn't had its answer
  uint32_t lastPageMs_ = 0;
};

// How much of the time the radio spends looking for the headphones
// (paging or inquiry), per minute: sampled from BtAppT's ticks (a few
// times a second), for the [stats] bt line and the power probe. The old
// cycle kept it at ~85-100% with the headphones away; resting, ~0.
class RadioMeter {
public:
  static constexpr uint32_t kMinuteMs = 60000;
  // A gap longer than this between samples (BtAppT busy, ticks lost) counts
  // only this much of it, as whatever the last sample said.
  static constexpr uint32_t kMaxStepMs = 2000;

  void sample(uint32_t nowMs, bool busy);
  // The last full minute's share busy, 0-100; -1 until a minute has passed.
  int lastMinutePercent() const { return lastPercent_; }
  // Busy time in the minute in progress, ms.
  uint32_t busyMsThisMinute() const { return busyMs_; }

private:
  bool started_ = false;
  bool lastBusy_ = false;
  uint32_t lastMs_ = 0;
  uint32_t minuteStartMs_ = 0;
  uint32_t countedMs_ = 0;  // the part of this minute sampled
  uint32_t busyMs_ = 0;
  int lastPercent_ = -1;
};
