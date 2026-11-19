// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "WallClock.h"

// The board's clock against its RTC's seconds (docs/HOST-STATUS.md,
// "@time"): what app/BoardClock does with the chip from the loop, a pass at
// a time, never waiting on it.
//
// The chip counts whole seconds and its fraction can't be read, so the
// boot's reading says only that the time is in that second
// (wallclock::Clock::boot() takes its start: up to a second behind). Its
// next tick is exact: the boot's catch reads the chip every kPollMs until
// its second changes, and the clock is the new second (plus the phase
// below) at the moment between the last read of the old second and the
// first of the new one (within half the gap between the two).
//
// Where the chip's seconds begin depends on what it does at a write
// (ESTIMATED either way: the BM8563's datasheet doesn't say; the PCF8563's
// divider runs on through one). One that restarts its divider at a write
// begins its seconds where the write was; one that doesn't keeps its own,
// any fraction of a second from the true ones. So a @time (the clock set at
// once, Clock::set()) writes the RTC on a second: when the clock it set
// reaches its next whole second, on the first loop pass then (a pass more
// than kLateMs late waits for the next second, kWaits times at most). Then
// it catches the chip's next tick as the boot does, against the clock the
// @time set: the phase, how far after the true seconds the chip's begin
// (the write's lateness, a few ms, for the first kind; anything within a
// second for the other). The phase is kept (NVS "clock"/"phase",
// app/BoardClock) and each boot's catch adds it, so a restart reads the
// time within the two catches' brackets (a few tens of ms) either way, plus
// the chip's drift while off. It is dropped just before the write and kept
// once measured: a restart in between leaves none (0 added: within the
// write's lateness for the first kind, within a second for the other),
// never an older write's.
//
// The chip is read in one run of its seven time registers (02h-08h), the
// boot's read's burst, and written in WallClock's rtcWrites() steps;
// nothing longer goes on the bus it shares with the touch panel and the
// AXP192.
// Portable: host-tested in test_rtc_sync, against a model of the chip that
// restarts its divider at a write and one that doesn't.
namespace rtcsync {

// A catch reads the chip at most this often (a loop pass is 5-25 ms).
inline constexpr uint32_t kPollMs = 10;
// A tick between two reads this close ends a catch (its moment their
// middle: within half of it).
inline constexpr uint32_t kCloseMs = 30;
// A catch's longest: three of the chip's ticks. The closest bracket seen
// by then is taken (a slow pass, reads that didn't go through).
inline constexpr uint32_t kCatchMs = 3300;
// The same second read for this long: the chip isn't counting. After a
// write, longer: a chip may hold its first tick after one (the PCF8563's
// first after its STOP bit comes half a second on; ESTIMATED: the
// BM8563's after a write, anything up to a second more).
inline constexpr uint32_t kStillMs = 1100;
inline constexpr uint32_t kStillAfterWriteMs = 2100;
// A @time's write later than this after its second waits for the next
// one, kWaits times at most.
inline constexpr uint32_t kLateMs = 30;
inline constexpr int kWaits = 2;
// Writes a @time tries before it gives up (each on a second).
inline constexpr int kAttempts = 3;
// The phase, ms: none kept, and the most a measured one can be.
inline constexpr int32_t kNoPhase = INT32_MIN;
inline constexpr int32_t kMaxPhase = 2000;

// A phase as NVS holds it: kNoPhase when absent or out of range.
int32_t phaseFrom(bool have, int32_t stored);

// One read of the chip's time registers.
struct Sample {
  bool answered = false;  // the I2C read went through
  bool believed = false;  // a time the boot would believe (WallClock's judge())
  bool voltLow = false;   // its voltage-low flag
  uint64_t second = 0;    // that time, Unix ms (when believed)
};
// `regs` 02h-08h, as read (when `answered`).
Sample sample(bool answered, const uint8_t (&regs)[7]);

// What the loop does this pass (BoardClock), and tells back.
enum class Do : uint8_t {
  Nothing,
  Read,    // the time registers: read()
  Forget,  // the kept phase dropped (NVS): forgot()
  Write,   // the RTC set to `second` (rtcWrites()): wrote()
};

// What came of it, for the log (and the phase to keep).
struct Event {
  enum class Kind : uint8_t {
    None,
    Caught,   // the boot's catch: the clock is the chip's tick (refined)
    Missed,   // the boot's catch gave up: the clock as the boot read it
    Taken,    // a @time's write taken and its tick caught: keep phaseMs
    Untimed,  // a @time's write taken, its tick not caught: no phase kept
    Refused,  // a @time's write failed, or its read-back isn't it
  };
  enum class Why : uint8_t {
    None,
    Still,        // Missed, Untimed: its second unchanged for kStillMs (kStillAfterWriteMs)
    Unanswered,   // Missed, Untimed: too few reads went through in kCatchMs
    Disagreed,    // Missed: its tick isn't within a second of the boot's reading
    WriteFailed,  // Refused: a write didn't go through
    Unread,       // Refused: no read-back went through in kCatchMs
    Flag,         // Refused: the read-back's voltage-low flag still set
    NotTime,      // Refused: the read-back isn't a time believed (19yy, not a date)
    Wrong,        // Refused: the read-back isn't the second written (or one since)
  };
  Kind kind = Kind::None;
  Why why = Why::None;
  uint64_t second = 0;   // Caught: the second the tick began; the others: the second written
  uint64_t time = 0;     // Caught: the clock at that tick (the second and the phase)
  int64_t shiftMs = 0;   // Caught: the clock moved by this (the boot's reading was this far behind)
  uint32_t withinMs = 0; // Caught, Taken: half the bracket the tick fell in
  int32_t phaseMs = kNoPhase;  // Caught: the phase added (kNoPhase: none kept, 0); Taken: measured
  uint32_t lateMs = 0;   // Taken, Untimed, Refused: how long after its second the write went
  int attempt = 0;       // Refused: the write's number (1 to kAttempts)
  bool again = false;    // Refused: written again at the next second
  Sample back;           // Refused: the read-back
};

class Sync {
public:
  // The boot, after Clock::boot(): when the clock is the RTC's, the catch
  // from the next pass, with the kept phase (kNoPhase: none).
  void boot(const wallclock::Clock& clock, int32_t phase);
  // A @time set the clock (Clock::set() took `unixMs` at `mono`): the RTC
  // written when it reaches its next whole second (at once on one; 2099's
  // last second, the next being past the chip's calendar, at once). The
  // latest wins: anything under way is dropped.
  void plan(uint64_t unixMs, uint64_t mono);
  // This pass's at `mono`: Write's second in *second. Called again after
  // each Forget and Write in the same pass (Read once a pass).
  Do step(uint64_t mono, wallclock::Clock& clock, uint64_t* second);
  void forgot();
  void wrote(uint64_t mono, bool ok, const wallclock::Clock& clock);
  // The read a Read asked for, at `mono` (taken just before it).
  void read(uint64_t mono, const Sample& s, wallclock::Clock& clock);
  bool busy() const { return mode_ != Mode::Idle; }
  // The latest event since the last call (one at most a call to the above).
  bool takeEvent(Event* out);

private:
  enum class Mode : uint8_t { Idle, Catch, Wait, Writing };
  enum class Purpose : uint8_t { Boot, Check };
  struct Bracket {
    bool valid = false;
    uint64_t tick = 0;    // its middle (mono)
    uint64_t second = 0;  // the second it began
    uint64_t gap = 0;     // between the two reads
  };
  void startCatch(Purpose p, uint64_t mono, bool started);
  void finish(Event::Why why, wallclock::Clock& clock);
  void refuse(Event::Why why, const Sample& back, uint64_t mono, const wallclock::Clock& clock);
  void post(const Event& e);

  Mode mode_ = Mode::Idle;
  Purpose purpose_ = Purpose::Boot;
  int32_t phase_ = kNoPhase;  // the boot's, kept by the last write
  // The catch.
  bool started_ = false;
  uint64_t start_ = 0;
  bool polled_ = false;
  uint64_t lastPoll_ = 0;
  bool have_ = false;      // a believed read of the current second
  uint64_t last_ = 0;      // that second
  uint64_t firstMono_ = 0, lastMono_ = 0;  // its first and latest read
  bool checked_ = false;   // Check: the read-back taken
  Bracket best_;
  // The write.
  uint64_t writeAt_ = 0;   // mono
  int waits_ = 0;
  int attempt_ = 0;
  bool forgotten_ = false;
  uint64_t written_ = 0;   // the second
  uint64_t writtenMono_ = 0;
  uint32_t lateMs_ = 0;
  bool pending_ = false;
  Event event_;
};

}  // namespace rtcsync
