// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The card worker's scheduler (docs/METADATA.md 3.3.3-3.3.5; milestone N7):
// what the one worker on the card does next, and when the background work
// yields to the music and the listener. The worker (N10: Thumbs' worker,
// generalised) takes one step at a time, and only the step it is handed: a
// cover; a waveform's read (docs/WAVEFORM-INDEX.md 11); the update step's
// build or its save (3.4.2, N12); a slice of the validation walk (3.2.3: its
// folders, or passes of a big one, for about 18 ms while the screen is lit,
// 250 ms while it is dark: the firmware's CardTasks); a compaction of
// tags.bin (3.3.2); a file of the scan (3.3.1) a loop source names, or a
// slice of its rest's files; the listening stats' step (docs/LISTENING-
// STATS.md 6.8, S2): a track's read, or a rank. The loop calls update() every pass, where the
// inputs are, and hands the worker what it says at the priority it says. So
// the jobs never overlap (a build, a compaction and a scan never run at
// once), and each step starts only when nothing it would disturb is under
// way.
//
// What starts, when the worker is free (the first that applies):
//   1. The update step's build, at priority 1 (the loop's): the listener
//      waits for it behind the "Updating library" fence. Nothing here holds
//      it: LibraryUpdate's safe point decides when it is asked (N12).
//   2. A list is moving (on a lit screen: listHolds()), or a finger is on
//      its A-Z rail (a scrub, or resting on it before one): nothing else
//      starts, covers and waveforms included (Thumbs' rule).
//   3. A waveform's read, then the listening stats' track read, then a
//      cover, one lane: at priority 1, dropped to 0 while a list moves under
//      it (Thumbs' rule): rows on screen come first (3.3.3, 0). The audio's
//      yields below don't hold them. The reads go first: a few small reads
//      (the stats' a binary search of the base), where a cover may be a
//      decode.
//   4. The update step's save of library.idx, at priority 0, 1 while the
//      screen is dark (as the scan's slices): it ends the update step. It
//      yields as the background work does, but not to the battery floor
//      (the build is paid for).
//   5. The background work, one job at a time, the first with work: the
//      walk, a compaction, the listening stats' rank, the scan. None of them
//      starts while the update step holds the worker (from its build's fence
//      to its save's end: nothing writes tags.bin or the journals while the
//      build streams them, 3.4.2; the rank so starts after library.idx's
//      save). At priority 0, but for four (3.3.9, 2026-10-09): a
//      slice of the walk at 1, as a cover (seconds once a boot, while the
//      Library tab says "Checking the card..."); a slice of the scan, a
//      compaction and a rank, at 1 while the screen is dark (In::dark: the
//      loop has nothing to draw). A rank is one step, never cut: it reads
//      the whole base (467 KB at 19,478 played) holding the SPI bus and
//      FatFs's volume lock against the decoder, so it never starts under a
//      yield, and at 1 while lit it would time-slice with the loop for its
//      whole step. At 0 they share what the loop leaves with the idle
//      task, half of it (covers measured 2-2.5x slower at 0; the save
//      4.5-9.9 s on the worker against 2.2 s on the loop at boot, the
//      2026-10-09 device run). A compaction, a save or a rank under way
//      drops to 0 the pass the screen lights (it isn't a slice: it runs on
//      to its end, below the loop).
// A slice under way drops to 0 and is cut after its unit (Out::cut) the
// moment a list moves or a wait below applies: so a wait holds the walk and
// the scan within one unit (a folder, a file) and a pass, as before slices.
// A list holds nothing while the screen is dark (In::dark: listHolds()):
// none is seen moving then, whatever its page still owes the wake (a list
// reloaded in the dark owes a frame until then: on 2026-10-14 the library
// update's save waited 260 s for "a list moving" with the screen off).
// The scan's next file comes from the first source that has one (3.3.3):
// the playing track, the queue's next kQueueNext entries, the kQueueSoon
// after them, the album or folder the Library tab shows, then the rest in
// D's canonical order.
//
// The save and the background work wait (3.3.4, 3.3.5) for the first of:
//   Battery     the scan only (U13): below 10% off USB it pauses, and goes
//               on once on USB or above 15%. The walk, a compaction, the
//               save, covers, waveforms and the build don't: they are short,
//               and they are what the listener sees.
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
//               event (up or down); the headphones' radio link up with
//               A2DP not yet over it, at most 8 s from its rise, then the
//               3 s after it ends (or after the 8 s); and a link up whose
//               AVRCP isn't yet, at most 10 s from the link, then the 3 s
//               after AVRCP comes up (or after the 10 s).
// Playing doesn't hold it, nor does the battery above the floor: the scan
// runs on battery and while playing (the user's choice, 3.3.5). The times
// are ESTIMATED; L3 and L5 measure them (Config), but for the wait for
// AVRCP, whose reason is measured (below).
//
// The wait for AVRCP (2026-11-19). Bluedroid opens AVRCP itself 3.5 s after
// A2DP opens (BTA_AV_RC_DISC_TIME_VAL, compiled into the shipped libbt.a): an
// SDP search first, a dip of about 5.5 KB of internal RAM for a moment
// (MEASURED: the worker's sampled lowest against the lowest since the boot;
// by code a 4,096 B response list, a 1,000 B discovery record and an L2CAP
// channel, ESTIMATED). On Powerbeats Pro AVRCP came up 3.73-3.80 s after the
// link in every boot recorded on main (about 3.8 s on rd-g5-fix2 too; on
// rd-g5-fix1 they opened it themselves, with the link: "The radio link's
// set-up" below), so the 3 s settle let the boot's walk start
// 0.73-0.80 s before it (0.5 s before the timer, by code): the walk's worker
// (its 7.5 KB stack) and AVRCP's set-up met, and with an MP3 playing in
// silent mode internal RAM fell to 35,916 B (G5's session floor is 43,008 B).
// So a link holds the background work until AVRCP is up (In::btControlUp),
// and AVRCP's coming up is a link event of its own (the settle after it);
// headphones that never open it hold it Config::btControlWaitMs, then the
// settle. A link that drops before AVRCP ends the wait (its own event holds
// the settle, as before). An AVRCP that comes after the cap, or connects
// again later on the link, is a link event too, held 3 s after it (so past
// the link + 13 s). A btSeq that moved while the link was up at both passes
// is a drop and a relink between them (BtSink counts the flips where they
// happen): a new link, whose wait starts again. Out::btControlWait says the
// wait holds: the card worker's idle task goes at once then (mayRelease(),
// app/CardTasks). A link that comes mid-walk cuts the slice, and the idle
// exit alone (3 s) ends the task at about the link + 3.0-3.3 s, just before
// AVRCP's set-up; release() makes that margin the whole wait, and ends a task
// a cover made in it. Not covered: a compaction, the update step's save or
// its build already under way at the link isn't cut (only a slice is), and
// the build isn't held at all: they still meet AVRCP's set-up. An AVRCP after
// the cap, or one that connects again mid-link, cuts a slice and holds 3 s,
// but the worker isn't released (the wait is over), and its own SDP search
// comes with its connect (bta_av_rc_opened()), before the cut.
//
// The radio link's set-up (2026-11-19). Before A2DP's link the headphones'
// radio link (their ACL) comes up, and the link's set-up runs over it: SDP,
// A2DP, and AVRCP with it when they open AVRCP too (rd-g5-fix1), else
// Bluedroid's own AVRCP about 3.5-3.8 s after the link (rd-g5-fix2). A2DP
// came about 0.6 s and 1.9 s after a radio link the headphones opened,
// 0.72 s after a page of ours was answered (MEASURED, three boots, the
// log's arrival times). A page burst or a pairing held the work through
// that (btSetup), but nothing held a radio link the headphones opened
// after the burst, backing off or resting: on the device (2026-11-19,
// rd-g5-fix1) the walk the burst's settle released started about 0.15 s
// into one, A2DP and AVRCP came up together about 0.6 s after it, and
// internal RAM went to 42,544 B, under G5's session floor (43,008 B). So
// the remembered headphones' radio link up with A2DP not up over it
// (In::btLinkSetup, BtSink::linkSetups(), RadioLinkNotes) holds the work as
// btSetup does, at most Config::btLinkSetupMs from its rise, then the
// settle; its end within that holds the settle from the pass that sees it.
// A page of ours over a radio link already up (A2DP opened over it: a
// back-off's page over one that lingers) is a set-up again, its own cap
// from the page. The pass that sees the set-up's end sees the link's rise
// too (BtSink counts the rise first, the loop reads the set-up first); a
// pass may see the link before the end, holding both waits for that pass;
// either way the wait for AVRCP takes over with no pass between them. A
// new radio link, a rise or a count that moved while it read up at both
// passes (In::btLinkSetupSeq), waits again from its own rise; one that
// came and went between two passes holds the settle. The cap ends the
// wait for that radio link for good, and its end after the cap holds
// nothing: a radio link that never brings A2DP (one stayed up 30.3 s)
// holds the work 8 + 3 s, not for good. While it holds within the cap the
// worker's idle task goes at once too (Out::btLinkSetupWait,
// mayRelease()): the set-up takes 0.6-2 s, shorter than the worker's 3 s
// idle exit, so a radio link that comes mid-walk would otherwise still
// meet its stack. A cover asked meanwhile makes the task again, and the
// next pass with nothing handed ends it: at most for the cap. Not covered:
// a relink over a radio link that stayed up after A2DP dropped (no new
// radio link: the drop's 3 s only); radio links that open again and again
// without ever bringing A2DP: each new one waits again from its own rise,
// so short ones (2 s every 5 s) would hold the work, the save too, for as
// long as they go on (none seen; before the set-up's wait nothing held
// them outside a burst). Bounding that across radio links (a set-up that
// came to nothing making the next ones within some time hold only their
// rise's settle, or holding no more after a few in a row) is the owner's
// call.
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
  // The worker's jobs. A step: a cover; a waveform's read; the build (one
  // step); the save (one step); a slice of the walk (its folders, or passes
  // of a big one); a compaction (one step); a file of the scan from a loop
  // source, or a slice of its rest; the listening stats' step, a track's read
  // or a rank (one step each: Out::statsRank says which).
  enum class Job : uint8_t { None, Cover, Wave, Build, Save, Walk, Compact, Scan, Stats };
  static constexpr int kJobs = 9;
  // Where the scan's next file comes from (3.3.3).
  enum class Source : uint8_t { None, Playing, QueueNext, QueueSoon, Shown, Rest };
  // What the worker's next start waits for (the first that applies, in this
  // order), or None: it starts Out::job, or there is nothing to do.
  enum class Wait : uint8_t {
    None,
    Step,         // a step is under way: one at a time
    List,         // a list is moving
    Updating,     // the update step holds the worker until its save
    Battery,      // below the floor, off USB (the scan)
    Input,        // input in the last Config::inputQuietMs
    Ring,         // playing, the ring below Config::ringMinPct
    Underrun,     // an underrun in the last Config::underrunBackoffMs
    DecodePass,   // a decode pass over Config::passLimitUs, in the last passBackoffMs
    TrackChange,  // the decoder at the heard track's end, until trackSettleMs into the next
    Seek,         // a seek under way, and seekSettleMs after
    Bluetooth,    // a pairing or link setup, btSettleMs after a link event; a radio link's set-up;
                  // a link's wait for AVRCP
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
    // The longest a link waits for its AVRCP (In::btControlUp), from the
    // link; then btSettleMs, as if AVRCP had come then. AVRCP came 3.73-3.80
    // s after the link on Powerbeats Pro (MEASURED, four boots), and at
    // about 7 s on the same headphones once a stream had started before it
    // (docs/ARCHITECTURE.md's late probe): a listener's order, PlayGate
    // starting the play with the link. 0: no wait (the link's own settle).
    uint32_t btControlWaitMs = 10000;
    // The longest a radio link being set up holds the work
    // (In::btLinkSetup), from its rise; then btSettleMs, as if it had ended
    // then. A2DP came about 0.6-1.9 s after the headphones' own radio link
    // and 0.72 s after a page of ours was answered (MEASURED, 2026-11-19).
    // BtSink leaves their radio link to them for 4 s (kTheirSetupMs), then
    // opens A2DP over it itself with a burst's page, which holds the work
    // from there (btSetup), and is a set-up again from the page (its own
    // cap); in a burst already under way its next page goes over their
    // link. Twice that 4 s: their whole allowance, and as long again for
    // the Core2's own open over it should no page hold the work.
    // A radio link that never brought A2DP stayed up 30.3 s (2026-11-19): it
    // holds the work 8 s and the settle, not for good. 0: the rise's settle
    // alone.
    uint32_t btLinkSetupMs = 8000;
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
    bool wave = false;      // a waveform's read is asked for (Now Playing's track)
    bool walk = false;      // the validation walk has folders left
    bool compact = false;   // a compaction is wanted (the journal's limits, a scan's end, before a build)
    // The listening stats (docs/LISTENING-STATS.md 6.8): a track's read is
    // asked (the console's Hp: a binary search of the base), in the waveform
    // read's lane after it; a rank is asked (Hr, Hl, Hw: the base, stats.bin's
    // TRKD and RCNT), background work after a compaction.
    bool statsTrack = false;
    bool statsRank = false;
    bool runningRank = false;  // the Stats step under way is a rank (its priority: as a compaction's)
    // The scan's sources (3.3.3), each true when it has a file to read. The
    // glue sets them only once the scan may run (D's to-do is known).
    bool playingPending = false;    // the playing track is Pending
    bool queueNextPending = false;  // one of the queue's next kQueueNext entries is
    bool queueSoonPending = false;  // one of the kQueueSoon after them is
    bool shownPending = false;      // one in the album or folder the Library tab shows
    bool restPending = false;       // D's to-do has one (canonical order)
    // ---- the UI ----
    bool listMoving = false;  // a list scrolls, flings or follows a finger (Page::animating()), or a finger is on
                              // its A-Z rail (Ui::listMoving()); none while `dark`
    bool input = false;       // any input this pass (a touch, a button, PWR, a headphone key, the console; a
                              // list moving counts, so the quiet window follows a fling's end)
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
    // The link events (up, down), free-running: counted where they happen
    // (BtSink), so a drop and a relink between two passes still show, as a
    // change while btLinked was up at both (a new link: its wait again).
    uint32_t btSeq = 0;
    bool btLinked = false;     // the A2DP link is up (its rise starts the wait for AVRCP)
    bool btControlUp = false;  // AVRCP is up on that link (BtSink::avrcpUp(): false while unlinked)
    // The remembered headphones' radio link (their ACL) is up and A2DP isn't
    // up over it yet (BtSink::linkSetups()'s low bit): the link's set-up,
    // held as btSetup is, capped by Config::btLinkSetupMs from its rise.
    bool btLinkSetup = false;
    // Its rises and ends, free-running (BtSink::linkSetups()): a new radio
    // link over one never seen to close, or a page of ours over one up,
    // counts two, so a count that moved while btLinkSetup read up at both
    // passes is a new set-up (its cap again), and one that, read down now,
    // moved by more than the end of the set-up read up at the last pass
    // (one; none if it read down) is a set-up that came and went between
    // them (the settle).
    uint32_t btLinkSetupSeq = 0;
    // ---- power ----
    bool usb = false;  // external power (ACIN or VBUS); unknown counts as present
    int battery = -1;  // percent (0-100); negative: not known (the floor keeps its state)
  };

  struct Out {
    Job job = Job::None;           // hand the worker one step of this now (None: nothing)
    Source source = Source::None;  // job Scan: where its file comes from
    uint8_t priority = kLowPriority;  // the worker's priority: the running step's, else `job`'s
    Wait wait = Wait::None;        // why nothing starts (None: `job` starts, or nothing to do)
    bool statsRank = false;        // job Stats: a rank (false: a track's read)
    // The step under way is a slice of the walk or the scan, and a list
    // moves or a wait applies (the scan's battery floor included): it ends
    // after its unit (CardJobs::cutSlice()), at priority 0 meanwhile.
    bool cut = false;
    bool batteryHeld = false;      // the floor holds the scan from this pass (the log, the status line)
    bool batteryReleased = false;  // ... and lets it go from this one
    // A link is up and its AVRCP isn't yet, within Config::btControlWaitMs:
    // the worker's task may go now, idle (mayRelease(), CardWorker::
    // release()), its stack free before AVRCP's set-up.
    bool btControlWait = false;
    // A radio link is being set up (In::btLinkSetup), within
    // Config::btLinkSetupMs of its rise: the worker's task may go now, idle,
    // its stack free while SDP, A2DP and AVRCP are set up over it. Not in
    // the settle after its end or after the cap.
    bool btLinkSetupWait = false;
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
  // The card worker's idle task may end now (CardWorker::release(),
  // app/CardTasks): a radio link is being set up or a link waits for its
  // AVRCP (o.btLinkSetupWait, o.btControlWait: each capped, so a task that
  // covers make and this ends again goes on at most that long; not a page
  // burst's or a pairing's wait, which runs for tens of seconds and would
  // end and remake it between every two covers), the worker is free
  // (`workerFree`: no step under way or unpolled, gs not holding it),
  // nothing is handed this pass (a cover, a waveform's read or the build
  // would make it again at once), and no update step is asked
  // (`updateAsked`: its build must find the task, ensure()).
  static bool mayRelease(const Out& o, bool workerFree, bool updateAsked) {
    return (o.btLinkSetupWait || o.btControlWait) && workerFree && o.job == Job::None && !updateAsked;
  }
  // A step's priority (3.3.4, 3.3.9) when no wait applies: the build 1; a
  // cover, a waveform's read, the stats' track read (Job::Stats) and the
  // walk 1, 0 while a list moves; the scan, a compaction and the save 1
  // while the screen is dark, else 0. (A list "moving" in the dark is none:
  // listHolds().) A rank: rankPriority().
  static uint8_t priorityOf(Job job, bool listMoving, bool dark = false);
  // The stats' rank (Job::Stats, Out::statsRank): a compaction's, 1 while
  // the screen is dark, else 0.
  static uint8_t rankPriority(bool listMoving, bool dark) { return priorityOf(Job::Compact, listMoving, dark); }
  // A list that moves holds work (the wait List, a slice cut, a cover, a
  // waveform's read or the walk below the loop) only while the screen is
  // lit: in the dark (In::dark: nothing drawn) no list is seen moving, and
  // one that owes the wake its frame (a reload, a fling the screen's off
  // stopped) holds nothing. The console's gs paces its pieces by it too
  // (lib/core TagCheck). Before 2026-10-14 a list reloaded in the dark held
  // the update step's save 260 s, until the screen woke.
  static bool listHolds(bool listMoving, bool dark) { return listMoving && !dark; }

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
  // The link's wait for AVRCP: the link seen up at the last pass, since
  // linkAtMs_ (its rise, or a relink btSeq showed); the wait holds (AVRCP
  // not up, the cap not reached); AVRCP at the last pass.
  bool linked_ = false;
  bool controlWait_ = false;
  bool lastBtControlUp_ = false;
  uint32_t linkAtMs_ = 0;
  // The radio link's set-up: seen at the last pass, since setupAtMs_ (its
  // rise, or a new radio link btLinkSetupSeq showed); the wait holds (not
  // ended, the cap not reached).
  bool setup_ = false;
  bool setupWait_ = false;
  uint32_t setupAtMs_ = 0;
  uint32_t lastBtLinkSetupSeq_ = 0;
  bool batteryLow_ = false;
  uint64_t waited_[kWaits] = {};
  uint32_t steps_[kJobs] = {};
  uint64_t stepMs_[kJobs] = {};
  uint32_t stepMaxMs_[kJobs] = {};
  uint32_t units_[kJobs] = {};
};
