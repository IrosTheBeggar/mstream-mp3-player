// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The card worker's scheduler (docs/METADATA.md 3.3.3-3.3.5; milestone N7):
// what the one worker on the card does next, and when the background work
// yields to the music and the listener. The worker (N10: Thumbs' worker,
// generalised) takes one step at a time, and only the step it is handed: a
// cover; the update step's build or its save (3.4.2, N12); a slice of the
// validation walk (3.2.3: its folders, or passes of a big one, for about
// 18 ms while the screen is lit, 250 ms while it is dark: the firmware's
// CardTasks); a compaction of tags.bin (3.3.2); a file of the scan
// (3.3.1) a loop source names, or a slice of its rest's files; the DJNB
// check. The loop calls update() every pass, where the inputs are, and
// hands the worker what it says at the priority it says. So the jobs never
// overlap (a build, a compaction and a scan never run at once), and each
// step starts only when nothing it would disturb is under way.
//
// What starts, when the worker is free (the first that applies):
//   1. The update step's build, at priority 1 (the loop's): the listener
//      waits for it behind the "Updating library" fence. Nothing here holds
//      it: LibraryUpdate's safe point decides when it is asked (N12).
//   2. A list is moving: nothing else starts, covers included (Thumbs' rule).
//   3. A cover, at priority 1, dropped to 0 while a list moves under it
//      (Thumbs' rule): rows on screen come first (3.3.3, 0). The audio's
//      yields below don't hold it.
//   4. The update step's save of library.idx, at priority 0, 1 while the
//      screen is dark (as the scan's slices): it ends the update step. It
//      yields as the background work does, but not to the battery floor
//      (the build is paid for).
//   5. The background work, one job at a time, the first with work: the
//      walk, a compaction, the scan, the DJNB check. None of them starts
//      while the update step holds the worker (from its build's fence to
//      its save's end: nothing writes tags.bin or the journals while the
//      build streams them, 3.4.2). At priority 0, but for three (3.3.9,
//      2026-10-09): a slice of the walk at 1, as a cover (seconds once a
//      boot, while the Library tab says "Checking the card..."); a slice
//      of the scan, and a compaction, at 1 while the screen is dark
//      (In::dark: the loop has nothing to draw). At 0 they share what the
//      loop leaves with the idle task, half of it (covers measured 2-2.5x
//      slower at 0; the save 4.5-9.9 s on the worker against 2.2 s on the
//      loop at boot, the 2026-10-09 device run). A compaction or a save
//      under way drops to 0 the pass the screen lights (it isn't a slice:
//      it runs on to its end, below the loop).
// A slice under way drops to 0 and is cut after its unit (Out::cut) the
// moment a list moves or a wait below applies: so a wait holds the walk and
// the scan within one unit (a folder, a file) and a pass, as before slices.
// The scan's next file comes from the first source that has one (3.3.3):
// the playing track, the queue's next kQueueNext entries, the kQueueSoon
// after them, the album or folder the Library tab shows, then the rest in
// D's canonical order.
//
// The save and the background work wait (3.3.4, 3.3.5) for the first of:
//   Battery     the scan and the DJNB check only (U13): below 10% off USB
//               they pause, and go on once on USB or above 15%. The walk, a
//               compaction, the save, covers and the build don't: they are
//               short, and they are what the listener sees.
//   Input       any input in the last 0.5 s: a tap's redraw comes first.
//   Ring        playing, with the PCM ring below half: a start's refill, a
//               dip.
//   Underrun    one in the last 30 s.
//   DecodePass  a decode pass over 40 ms in the last 5 s: G6's 30 ms
//               (docs/OPUS.md) and a margin; the decoder waited for the card
//               (the steps' reads hold the SPI bus and FatFs's volume lock).
//   TrackChange from the decoder's end of file on the heard track (it opens
//               the next into the same ring: docs/GAPLESS.md 2.1) until 2 s
//               into the next one; a start's or a skip's first 2 s too.
//   Seek        a seek under way, and the 2 s after it.
//   Bluetooth   a pairing or a link being set up, and the 3 s after a link
//               event (up or down).
// Playing doesn't hold it, nor does the battery above the floor: the scan
// runs on battery and while playing (the user's choice, 3.3.5). The times
// are ESTIMATED; L3 and L5 measure them (Config).
//
// The idle power-off and the sleep timer (which turns the power off only
// through the idle power-off) have no blocker for the scan: a power-off
// interrupts it, and the journal resumes it. The update step and a
// compaction hold IdlePolicy's LibraryWrite: the idle power-off never cuts
// them.
//
// Portable, host-tested (test_scan_scheduler); the firmware's card worker
// runs it (app/CardTasks, N10), and N12's LibraryUpdate asks for the build
// and the save and holds the rest from its fence to the save's end
// (test_library_update runs the two together).
class ScanScheduler {
public:
  // The worker's jobs. A step: a cover; the build (one step); the save (one
  // step); a slice of the walk (its folders, or passes of a big one); a
  // compaction (one step); a file of the scan from a loop source, or a
  // slice of its rest; the DJNB check (one step).
  enum class Job : uint8_t { None, Cover, Build, Save, Walk, Compact, Scan, DjCheck };
  static constexpr int kJobs = 8;
  // Where the scan's next file comes from (3.3.3).
  enum class Source : uint8_t { None, Playing, QueueNext, QueueSoon, Shown, Rest };
  // What the worker's next start waits for (the first that applies, in this
  // order), or None: it starts Out::job, or there is nothing to do.
  enum class Wait : uint8_t {
    None,
    Step,         // a step is under way: one at a time
    List,         // a list is moving
    Updating,     // the update step holds the worker until its save
    Battery,      // below the floor, off USB (the scan, the DJNB check)
    Input,        // input in the last Config::inputQuietMs
    Ring,         // playing, the ring below Config::ringMinPct
    Underrun,     // an underrun in the last Config::underrunBackoffMs
    DecodePass,   // a decode pass over Config::passLimitUs, in the last passBackoffMs
    TrackChange,  // the decoder at the heard track's end, until trackSettleMs into the next
    Seek,         // a seek under way, and seekSettleMs after
    Bluetooth,    // a pairing or link setup, and btSettleMs after a link event
  };
  static constexpr int kWaits = 12;

  // The worker's FreeRTOS priorities (3.3.4, 3.3.9): the loop's for what
  // the listener waits for (and the walk's slices, and the scan's slices,
  // a compaction and the save while the screen is dark),
  // the idle task's for the rest, so the loop preempts it whenever it is
  // ready (the SD driver's reads busy-wait the CPU).
  static constexpr uint8_t kHighPriority = 1;
  static constexpr uint8_t kLowPriority = 0;

  // The queue's sources (3.3.3): its next 3 entries, then the 200 after them.
  static constexpr uint32_t kQueueNext = 3;
  static constexpr uint32_t kQueueSoon = 200;

  // After a walk that found changes (3.3.3, U11): the update step runs at
  // once (the new files show, with their names) when the walk added this
  // many files or more, or the scan to follow is estimated over
  // kBuildNowScanMs; else it waits for the scan's end.
  static constexpr uint32_t kBuildNowAdded = 200;
  static constexpr uint32_t kBuildNowScanMs = 60000;
  // A file's step for the estimate until the scan has timed its own:
  // 3.3.4's 3-6 min for 20k files while playing, with the sector cache
  // (ESTIMATED; 7.5-8.7 ms idle; 42-69 ms without the cache).
  static constexpr uint32_t kEstimateMsPerFile = 18;

  struct Config {
    uint32_t inputQuietMs = 500;
    uint32_t ringMinPct = 50;
    uint32_t underrunBackoffMs = 30000;
    uint32_t passLimitUs = 40000;
    uint32_t passBackoffMs = 5000;
    uint32_t trackSettleMs = 2000;
    uint32_t seekSettleMs = 2000;
    uint32_t btSettleMs = 3000;
    // The battery floor (U13): held below floorPct off USB, until USB or a
    // reading above resumeAbovePct.
    int floorPct = 10;
    int resumeAbovePct = 15;
  };

  struct In {
    uint32_t nowMs = 0;
    // ---- the worker ----
    Job running = Job::None;  // the step under way; None: the worker is free
    // ---- what has work (N10's and N12's glue) ----
    bool build = false;     // the update step asks for its build (3.4.2, step 4)
    bool save = false;      // the update step asks for its save (step 6)
    bool updating = false;  // the update step holds the worker: its fence to its save's end
    bool cover = false;     // a cover is asked for (a row on screen)
    bool walk = false;      // the validation walk has folders left
    bool compact = false;   // a compaction is wanted (the journal's limits, a scan's end, before a build)
    bool djCheck = false;   // DJNB's check is due (once per commit)
    // The scan's sources (3.3.3), each true when it has a file to read. The
    // glue sets them only once the scan may run (D's to-do is known).
    bool playingPending = false;    // the playing track is Pending
    bool queueNextPending = false;  // one of the queue's next kQueueNext entries is
    bool queueSoonPending = false;  // one of the kQueueSoon after them is
    bool shownPending = false;      // one in the album or folder the Library tab shows
    bool restPending = false;       // D's to-do has one (canonical order)
    // ---- the UI ----
    bool listMoving = false;  // a list scrolls, flings or follows a finger (Page::animating())
    bool input = false;       // any input this pass (a touch, a button, PWR, a headphone key, the console)
    bool dark = false;        // the screen is dark: nothing to draw (a scan's slice, a compaction, the save at 1)
    // ---- the audio ----
    bool playing = false;         // the player is Playing: audio should flow
    uint32_t ringMs = 0;          // the PCM ring's audio now (bufferedMsNow())
    uint32_t ringCapacityMs = 0;  // the ring's size in ms (about 1,490); 0: unknown (no ring rule)
    uint32_t underruns = 0;       // the outputs' count, free-running
    uint32_t decodePassUs = 0;    // the longest decode pass finished since the last update() (0: none)
    bool decoderAtEnd = false;    // the decoder is past the heard track's end of file (the next ahead, or a drain)
    uint32_t trackSeq = 0;        // changes when another track begins to be heard (a join, a start's first audio)
    bool seeking = false;         // a seek under way (its request to its first audio)
    uint32_t seekSeq = 0;         // the seeks asked for, free-running (one between passes isn't missed)
    // ---- Bluetooth ----
    bool btSetup = false;  // a pairing (the Pair screen's scan, a pairing) or a link being set up (paging)
    uint32_t btSeq = 0;    // the link events (up, down), free-running
    // ---- power ----
    bool usb = false;  // external power (ACIN or VBUS); unknown counts as present
    int battery = -1;  // percent (0-100); negative: not known (the floor keeps its state)
  };

  struct Out {
    Job job = Job::None;           // hand the worker one step of this now (None: nothing)
    Source source = Source::None;  // job Scan: where its file comes from
    uint8_t priority = kLowPriority;  // the worker's priority: the running step's, else `job`'s
    Wait wait = Wait::None;        // why nothing starts (None: `job` starts, or nothing to do)
    // The step under way is a slice of the walk or the scan, and a list
    // moves or a wait applies (the scan's battery floor included): it ends
    // after its unit (CardJobs::cutSlice()), at priority 0 meanwhile.
    bool cut = false;
    bool batteryHeld = false;      // the floor holds the scan from this pass (the log, the status line)
    bool batteryReleased = false;  // ... and lets it go from this one
  };

  ScanScheduler() = default;
  explicit ScanScheduler(const Config& c) : config_(c) {}
  void setConfig(const Config& c) { config_ = c; }
  const Config& config() const { return config_; }

  // Every loop pass. The first call takes the counters as they are (no
  // window opens for them).
  Out update(const In& in);

  // The worker finished a step of `job` that took `ms` (its own clock) and
  // did `units` of its work (a slice's CardWalk steps, or files of the
  // scan: 0 when it only looked at rows): the scan's rate for the estimate
  // and the console.
  void stepDone(Job job, uint32_t ms, uint32_t units = 1);

  // The floor holds the scan now.
  bool batteryLow() const { return batteryLow_; }
  // The time update() spent waiting for `w` (Step: a step under way), since
  // the start or resetStats(): what was behind a slow scan (L3).
  uint64_t waitedMs(Wait w) const;
  // The steps of `job` done (stepDone()), their mean and longest ms, and
  // their units.
  uint32_t steps(Job job) const;
  uint32_t meanStepMs(Job job) const;  // 0: none yet
  uint32_t maxStepMs(Job job) const;
  uint32_t units(Job job) const;
  // The scan's ms a file (its steps' ms over the files they took) once it
  // has read one, else kEstimateMsPerFile.
  uint32_t scanMsPerFile() const;
  void resetStats();

  // The update step after a walk that found changes (3.3.3, U11): true,
  // build now; false, at the scan's end.
  static bool buildAfterWalk(uint32_t added, uint32_t toScan, uint32_t msPerFile = kEstimateMsPerFile);
  // `files` at `msPerFile`, saturated.
  static uint32_t scanEstimateMs(uint32_t files, uint32_t msPerFile);
  // The scan's source from In's flags (3.3.3's order).
  static Source sourceOf(const In& in);
  // A step's priority (3.3.4, 3.3.9) when no wait applies: the build 1; a
  // cover and the walk 1, 0 while a list moves; the scan, a compaction and
  // the save 1 while the screen is dark and no list moves, else 0; the
  // DJNB check 0.
  static uint8_t priorityOf(Job job, bool listMoving, bool dark = false);

  static const char* jobName(Job j);
  static const char* sourceName(Source s);
  static const char* waitName(Wait w);

private:
  // A window that holds until `until`, cleared once that has passed (so a
  // stale one can't come back when millis() wraps).
  struct Window {
    bool on = false;
    uint32_t until = 0;
    void open(uint32_t nowMs, uint32_t forMs);
    void expire(uint32_t nowMs);
  };
  void observe(const In& in, Out& o);
  void decide(const In& in, Out& o) const;
  Wait yieldOf(const In& in) const;

  Config config_;
  bool primed_ = false;
  uint32_t lastNowMs_ = 0;
  Wait lastWait_ = Wait::None;
  uint32_t lastUnderruns_ = 0, lastTrackSeq_ = 0, lastSeekSeq_ = 0, lastBtSeq_ = 0;
  Window input_, underrun_, pass_, track_, seek_, bt_;
  bool batteryLow_ = false;
  uint64_t waited_[kWaits] = {};
  uint32_t steps_[kJobs] = {};
  uint64_t stepMs_[kJobs] = {};
  uint32_t stepMaxMs_[kJobs] = {};
  uint32_t units_[kJobs] = {};
};
