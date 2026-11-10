// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardManifest.h"
#include "PlaybackController.h"
#include "TagStore.h"
#include "TrackCatalog.h"

// The play log, /.player/plays.jnl (docs/METADATA.md 2.20, MPPL v1): one
// 80-byte slot for each play of a card file under /music, appended at the
// play's end, which the transfer software reads in a card reader and
// reports to mStream, then acknowledges in its next commit (MSMF's ACKS,
// 2.5.6). tools/playlog.py is the reference; test_play_log makes its
// goldens (test/fixtures/card/plays) byte for byte. Portable, host-tested;
// app/PlayHistory is the firmware's glue (the board's clock, NVS, the card
// through CardFat, the loop's write).
//   - the slot (2.20.3): encode(), decode();
//   - Journal: the device's files (2.20.2, 2.20.4, 2.20.5) through N4's
//     file interface (tagstore::Fs: FatFs on the device). At a mount, the
//     cut-rename rule and NVS's epoch (a new one when NVS has none); then
//     the boot's scan, a few slots a call, of every valid slot of the log's
//     files: the boot rules (the seq and the boot count) and the trim by
//     the root's ACKS. At a play's end its seq in NVS first, then one
//     append and its f_sync, rotating under the cap with a drop note. A
//     question of the card that it fails (which files are there, on which
//     chains: Fs::entry()) answers nothing: nothing is removed, renamed
//     or made on it (settle(), the trim, the rotation, an append's new
//     plays.jnl). A slot the card failed is the same slot when it is tried
//     again (again(): its seq, its bytes), never a second one;
//   - Tracker: a play from its start to its end, as PlaybackController
//     tells them (its Plays) and the backend's position shows it: the time
//     played (pauses excluded), the pauses, the outcome, the source;
//   - Outbox: the plays waiting, one at a time to the journal: the played
//     file's size and time asked of the card, the slot's append; a play
//     the card failed kept at the head and tried again (three tries in
//     all), nothing behind it written meanwhile; the orderly end's flush,
//     each play once, to the first card failure or its deadline;
//   - WriteGate: when the loop may write a slot (or read the boot's scan):
//     never in the gapless window (the decoder reading the next track
//     ahead, until the join has settled), a start's refill, a seek, or a
//     ring under half; nor while a list moves (the loop draws its frames).
namespace playlog {

// ---------------------------------------------------------------------------
// The slot (2.20.3): 80 bytes, little-endian, its own CRC-32 (2.3.2).
// ---------------------------------------------------------------------------
constexpr uint32_t kSlot = 80;
constexpr uint32_t kMagic = cardcontract::fourcc("MPPL");
constexpr uint8_t kMajor = 1;
constexpr uint8_t kKindPlay = 1;
constexpr uint8_t kKindDrop = 2;
// The outcome (PlaybackController::Plays::End's values) and the source.
constexpr uint8_t kCompleted = 1, kSkipped = 2, kStopped = 3;
constexpr uint8_t kSourceUnsaid = 0, kSourceManual = 1, kSourceShuffle = 2, kSourceAutoDj = 3;
// The clock byte (wallclock::Source's values): unset, the RTC's, a host's.
constexpr uint8_t kClockUnset = 0, kClockRtc = 1, kClockHost = 2;
// A file's cap (2.20.2): 6,553 slots, 524,240 bytes; the two files
// together 1,048,480. Also what a reader reads of a file (2.20.6, item 1).
constexpr uint32_t kFileSlots = 6553;
constexpr uint32_t kFileBytes = kFileSlots * kSlot;
// The scans' reads (the boot's, a rotation's count): 32 slots, 2,560
// bytes, five whole sectors from a sector's start (80 and 512 meet every
// 2,560 bytes), so FatFs reads them straight, one card command a read.
constexpr uint32_t kReadBytes = 2560;

struct Slot {
  uint8_t kind = kKindPlay;
  uint8_t outcome = 0;
  uint8_t source = 0;
  uint8_t clock = 0;
  uint32_t seq = 0;
  uint16_t pauseCount = 0;
  uint64_t epoch = 0;
  uint8_t deviceId[6] = {};
  uint64_t startedAt = 0;  // Unix ms; 0 unknown
  uint32_t bootSeq = 0;
  uint32_t uptimeMs = 0;
  uint32_t playedMs = 0;  // a drop note's: `dropped`
  uint32_t durationMs = 0;
  uint64_t pathHash = 0;
  uint32_t size = 0;
  uint32_t fatTime = 0;
  uint32_t generation = 0;
};
// A drop note's (kind 2) outcome, source, seq, pauseCount and bytes 52-75
// are written 0 whatever `s` holds; a play's fields as given.
void encode(const Slot& s, uint8_t out[kSlot]);
// 2.20.6's verdict on one slot, in this order: Short (under 80 bytes),
// Magic, Newer (MPPL with another major: a newer firmware's), Crc, Kind (a
// valid slot of a kind this firmware doesn't know: `out->kind` holds it),
// Ok (`out` holds every field).
enum class Verdict : uint8_t { Ok, Short, Magic, Newer, Crc, Kind };
Verdict decode(const uint8_t* b, uint32_t n, Slot* out);
// 2.5.6: a play slot of a device and epoch at or below ACKS's seq for
// them (none: 0) is acknowledged.
bool acknowledged(const Slot& s, const cardcontract::msmf::Acks* acks);

// The log's names (2.20.2): plays.jnl (i 0), plays.1.jnl (1) and a later
// firmware's plays.2.jnl to plays.9.jnl, which a reader reads too; the
// twins a cut rename leaves, plays.xl1 to plays.xl9 (tagstore's twin
// names with this stem), which nobody reads.
constexpr int kNames = 10;
constexpr const char* kLogPath = "/.player/plays.jnl";
constexpr const char* kPrevPath = "/.player/plays.1.jnl";
constexpr tagstore::Names kTwinNames{kPrevPath, kLogPath, "/.player/plays"};
bool logName(int i, char* out, size_t cap);

// ---------------------------------------------------------------------------
// The device's files.
// ---------------------------------------------------------------------------
class Journal {
public:
  // NVS's part (2.20.4): the epoch (0: none), the last seq used and the
  // boot count, in its own namespace (nvslayout's "plays").
  struct Ids {
    uint64_t epoch = 0;
    uint32_t seq = 0;
    uint32_t boots = 0;
  };
  class Nvs {
  public:
    virtual bool load(Ids* out) = 0;  // false: NVS can't be read (no log this boot)
    virtual bool storeSeq(uint32_t seq) = 0;
    virtual bool storeBoots(uint32_t boots) = 0;
    // A new epoch: its seq and boot count (0) first, the epoch last, so a
    // cut between leaves none and the next boot makes another.
    virtual bool storeEpoch(const Ids& ids) = 0;

  protected:
    ~Nvs() = default;
  };
  // What the journal did (the firmware's log lines; the tests compare
  // them with tools/playlog.py's ops): a new epoch (n), a play written (n:
  // its seq) or not written at this try (Lost: again() may still write
  // it), a rename (a to b), a delete (a, why b: "rotation",
  // "acknowledged", "twin"), a drop note written (n: dropped), a file
  // renamed to a twin (a to b), no twin name free (a), a rename to a twin
  // the card refused (a to b; the reference's card refuses none), a
  // rotation that went on past a plays.1.jnl it couldn't read whole (a;
  // n: what it counted of it; the reference's card fails no read).
  enum class Op : uint8_t { Epoch, Play, Lost, Rename, Delete, Note, Twin, Blocked, Refused, Unread };
  using OnOp = void (*)(void* ctx, Op op, const char* a, const char* b, uint64_t n);
  struct Config {
    // A file's cap in slots: the contract's, or smaller (a test's 4).
    uint32_t capSlots = kFileSlots;
    // The scans' reads: at least kSlot bytes (a multiple of it reads the
    // most a call; PSRAM on the device).
    uint8_t* buf = nullptr;
    uint32_t bufBytes = 0;
    // A random u64 for a new epoch (the hardware RNG); 0 is drawn again.
    uint64_t (*random)(void* ctx) = nullptr;
    void* randomCtx = nullptr;
    OnOp onOp = nullptr;
    void* opCtx = nullptr;
  };

  Journal(tagstore::Fs& fs, Nvs& nvs, const Config& c) : fs_(fs), nvs_(nvs), c_(c) {}
  ~Journal() { closeScan(); }
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  // At the boot's mount, the card's records open: NVS's epoch (a new one
  // when it has none), the cut-rename rule (settle()), and the scan's
  // start; `acks` the root's (copied: the root doesn't change in a boot).
  // False: NVS can't be read or written, or no buffer: no log this boot.
  bool begin(const uint8_t deviceId[6], const cardcontract::msmf::Acks& acks);
  // The boot's scan, at most `maxSlots` slots of it (whole files' opens
  // aside): every valid slot of the log's files, for the boot rules (the
  // highest seq and bootSeq of this device and epoch) and the trim; none
  // read when the epoch is this boot's and the root has no ACKS. At its
  // end the boot count is stored (before any slot uses it), the trim done
  // (2.20.2, only with ACKS: every file they acknowledge whole, that no
  // twin shares), and the log ready. True: ready (or off).
  bool scan(uint32_t maxSlots);
  bool ready() const { return state_ == State::Ready; }
  bool off() const { return state_ == State::Off; }
  // The cut-rename rule (2.20.2) at a mount: plays.jnl and plays.1.jnl on
  // one chain sends plays.jnl to a twin name; a twin no other file of the
  // log shares goes. begin() and remounted() do it. False: no twin name
  // free (blocked: no slot until a disk check frees one), or the card
  // refused the rename (unsettled: append() tries it again first), or it
  // failed one of the rule's questions (which files are there, on which
  // chains: tagstore::Fs::entry()), which answers nothing: nothing is
  // renamed or removed on it, and it is unsettled too (unanswered()).
  bool settle();
  // The same card mounted again (the card guard's remount: pulled and put
  // back, or FatFs's own): settle() again before the next slot; and while
  // no slot has been asked for this boot, the boot's scan again from its
  // start, since what it read before the card went may be partial (a file
  // open across the remount reads nothing more, a read or an open the card
  // refused while it was out). Its boot rules come out the same or higher
  // (the boot count from NVS's at the mount), its trim by the same ACKS.
  void remounted();

  enum class Wrote : uint8_t {
    Ok,       // appended and synced
    Off,      // no log this boot (or the scan not done); again(): nothing to try again
    Nvs,      // NVS didn't take the seq: not written (2.20.4)
    Blocked,  // no twin name free: lost until a disk check
    Failed,   // the card failed it (a question, a write, a rename, a full card, the card gone): again() tries it
  };
  // One play's slot at plays.jnl's end (`play`'s own fields as the caller
  // has them: outcome, source, clock, pauseCount, startedAt, uptimeMs,
  // playedMs, durationMs, pathHash, size, fatTime, generation): the seq
  // taken and stored in NVS first, then the epoch, deviceId and bootSeq
  // filled in, the slot appended and synced. A trailing part under a slot
  // (a cut append) is written over: the slot goes at the slot boundary.
  // When the next slot would pass the cap, the rotation: plays.1.jnl
  // deleted (counting what in it no ACKS acknowledges), or renamed to a
  // twin when one shares its chain; plays.jnl renamed plays.1.jnl; a new
  // plays.jnl with the drop note first when the count isn't 0.
  Wrote append(Slot& play);
  // The last append() again, after the card failed it (Failed): the same
  // slot, byte for byte as append() made it (its seq; its startedAt as the
  // clock dated it then), so one play is never two slots that differ. Its
  // first try may have landed with only its sync or its close failing:
  // when the log's last slot holds those very bytes they are written over
  // themselves, where they are, and synced; else the slot goes at the
  // log's end as an append's does (the rule, the rotation and the drop
  // note as append()'s). Only the last append()'s slot can be tried again:
  // Off when it didn't end Failed.
  Wrote again();
  // The last append() ended Failed: again() has its slot to try.
  bool owes() const { return retry_; }
  // A rotation that can't read plays.1.jnl for its count leaves the log as
  // it was (a read the card fails now may work at the next try); at the
  // kUnreadTries-th in a row it goes on with what it could read, so one
  // sector gone bad in plays.1.jnl doesn't hold every later play back.
  static constexpr uint8_t kUnreadTries = 3;

  uint64_t epoch() const { return ids_.epoch; }
  uint32_t bootSeq() const { return bootSeq_; }
  uint32_t lastSeq() const { return seq_; }  // the last seq taken (0: none)
  bool newEpoch() const { return newEpoch_; }
  bool blocked() const { return blocked_; }
  bool unsettled() const { return unsettled_; }
  // ... unsettled because the card failed a question, not a rename.
  bool unanswered() const { return unanswered_; }
  // The boot's: what its scan found and did.
  struct Booted {
    uint32_t files = 0;       // the log's files read
    uint32_t slots = 0;       // slots read
    uint64_t bytes = 0;       // bytes read
    uint32_t cardSeq = 0;     // the highest seq of this device and epoch on the card (0: none)
    uint32_t cardBoot = 0;    // ... bootSeq
    uint32_t trimmed = 0;     // files ACKS let go
    uint32_t twins = 0;       // twins left (the card wants a disk check)
    uint32_t restarts = 0;    // scans begun again (remounted())
  };
  const Booted& booted() const { return booted_; }
  // The last rotation's drop note's count (0: it had none to write), the
  // note written with a later slot for a rotation the card failed too.
  uint32_t lastDropped() const { return lastDropped_; }
  // What a rotation counted and no drop note has said yet (its step 1 done,
  // the card failing a later step): the next slot written carries the
  // note. Held in memory, as the count always was (2.20.5).
  uint32_t owedDrops() const { return owed_; }

private:
  enum class State : uint8_t { Off, Scanning, Ready };
  void op(Op o, const char* a, const char* b, uint64_t n) {
    if (c_.onOp) c_.onOp(c_.opCtx, o, a, b, n);
  }
  // The journal's questions of the card (tagstore::Fs::entry()), the first
  // it fails noted: from there the answers say nothing, and nothing is
  // removed, renamed or made on them (PlayLog.cpp).
  class Asking;
  // Another file of the log (its names, the twins) shares `path`'s first
  // cluster (a cut rename's two entries on one chain). No answer once a
  // question failed (a.failed()).
  bool shared(Asking& a, const char* path);
  // `path` renamed to the first free twin name (a rename frees nothing);
  // false and blocked when all nine are taken, false (Refused) when the
  // card refuses the rename, false when it failed a question.
  bool toTwin(Asking& a, const char* path);
  void collectTwins(Asking& a);
  // The boot's scan from its start (the cut-rename rule done first).
  void startScan();
  // The scan's end: the boot rules, the trim.
  bool finishScan();
  void closeScan() {
    if (scanF_) fs_.close(scanF_);
    scanF_ = nullptr;
  }
  // The forward verdicts of one file of the scan.
  struct FileScan {
    uint32_t slots = 0, next = 0;
    bool past = false;    // longer than a reader reads (2.20.6, item 1)
    bool unjudged = false;  // a newer major's slot, or a valid one of an unknown kind
    bool anyPlay = false, allAcked = true, lastIsPlay = false;
  };
  // What a rotation's step 1 counts in plays.1.jnl (2.20.2): its valid
  // play slots ACKS doesn't acknowledge, and the `dropped` of each drop
  // note no acknowledged play follows. Saturating. False: unreadable (an
  // open or a read the card failed: *lost is what it counted before that).
  bool unread(uint32_t* lost);
  // The slot in last_ to the card (append()'s and again()'s), and its op.
  Wrote put(bool again);
  Wrote place(bool again);
  Wrote rotate();
  // [the drop note owed, when `note`][last_] into `out`: its bytes.
  uint32_t record(uint8_t out[2 * kSlot], bool note) const;
  // `n` bytes of a record at `at` in `f` (nullptr: its open failed),
  // synced and closed; a note written is owed no more.
  Wrote write(tagstore::File* f, uint32_t at, const uint8_t* rec, uint32_t n, bool note);
  bool newEpochNow();

  tagstore::Fs& fs_;
  Nvs& nvs_;
  Config c_;
  State state_ = State::Off;
  uint8_t dev_[6] = {};
  cardcontract::msmf::Acks acks_;
  Ids ids_;
  uint32_t bootSeq_ = 0;
  uint32_t seq_ = 0;
  bool newEpoch_ = false;
  bool blocked_ = false;
  bool unsettled_ = false;  // plays.jnl on plays.1.jnl's chain: the card refused its rename; or unanswered_
  bool unanswered_ = false;  // settle(): the card failed a question, nothing done on it
  bool appended_ = false;   // append() asked for this boot (its bootSeq may be on the card)
  uint32_t bootsAtMount_ = 0;  // NVS's boot count at begin() (the boot rule's, again at a rescan)
  // The scan.
  int scanFile_ = 0;
  tagstore::File* scanF_ = nullptr;  // scanFile_'s, open between pieces
  FileScan file_;
  uint32_t trimmable_ = 0;  // bit i: log file i may go
  Booted booted_;
  uint32_t lastDropped_ = 0;
  uint8_t last_[kSlot] = {};  // the last append()'s slot, as encoded
  bool retry_ = false;        // ... ended Failed: again() tries it
  uint32_t owed_ = 0;         // a rotation's count no drop note has said yet
  uint8_t unreadFails_ = 0;   // rotations in a row that couldn't read plays.1.jnl whole
};

// ---------------------------------------------------------------------------
// A play, from its start to its end.
// ---------------------------------------------------------------------------
// The file a play is of, as the firmware names it at the play's start
// (Tracker::Env).
struct PlayFile {
  uint64_t pathHash = 0;    // 2.3.3, of its path as the library has it
  uint32_t generation = 0;  // the root that counts (2.5.4): its generation; 0 none
  uint8_t source = kSourceUnsaid;
  uint32_t hintMs = 0;      // its length as the catalog knows it (0: none)
  char path[TrackCatalog::kMaxPath] = "";  // "/music/...": its size and FAT time are read at the write
};
// A play at its end, waiting for its slot.
struct Played {
  PlayFile file;
  uint64_t startMono = 0;  // the board's monotonic ms when it began
  uint32_t playedMs = 0;
  uint32_t durationMs = 0;  // as the backend knew it, else the hint; 0 unknown
  uint8_t outcome = kStopped;
  uint16_t pauses = 0;
};

class Tracker final : public PlaybackController::Plays {
public:
  class Env {
  public:
    // The board's monotonic ms (never wraps).
    virtual uint64_t monoMs() = 0;
    // The file the queue entry `key` (track `track`) names, at its play's
    // start: false when no slot is due for it (a built-in track, a file
    // not on the card).
    virtual bool file(uint32_t key, uint32_t track, PlayFile* out) = 0;

  protected:
    ~Env() = default;
  };
  // 2.20.5: plays of under a second of audio are left out.
  static constexpr uint32_t kMinPlayedMs = 1000;
  // Plays waiting for their slots (the loop writes one a pass when the
  // gate lets it): more than this, the newest is lost (counted). The gate
  // is shut for a start's first 2 s and a track's last 5, so a track of
  // 7 s or less never opens it, and a run of them waits whole: 32 of them
  // (8, before, was an album of interludes). A Played is about 300 bytes,
  // the ring 10 KB of the Tracker's PSRAM; all 32 at an orderly end are
  // well inside its flush's deadline (app/PlayHistory).
  static constexpr uint32_t kPending = 32;
  // The position moves at most this much more than the board's clock
  // between two looks for it to count as played (a seek's jump doesn't).
  static constexpr uint32_t kSlackMs = 500;
  // A play that ended at its end: the part of its last look's position to
  // its length counts too, up to this much (a pass's).
  static constexpr uint32_t kTailMs = 2000;

  explicit Tracker(Env& env) : env_(env) {}

  // PlaybackController's word.
  void began(uint32_t key, uint32_t track, uint32_t fromMs) override;
  void ended(End how) override;
  // Every loop pass (after the player's update(): a join is taken first):
  // the player's state and the backend's position and length.
  void tick(PlayState state, uint32_t positionMs, bool positionKnown, uint32_t durationMs);
  bool open() const { return open_; }
  // The plays waiting for their slots, oldest first.
  uint32_t pending() const { return count_; }
  const Played& oldest() const { return ring_[head_]; }
  void pop();
  struct Counts {
    uint32_t ended = 0;     // plays ended
    uint32_t notLogged = 0;  // ... of no card file
    uint32_t short_ = 0;    // ... under kMinPlayedMs
    uint32_t queued = 0;    // ... waiting for or given their slot
    uint32_t overflow = 0;  // ... lost: kPending were waiting
  };
  const Counts& counts() const { return counts_; }

private:
  Env& env_;
  bool open_ = false;
  bool logged_ = false;
  Played cur_;
  bool havePos_ = false;
  uint32_t lastPos_ = 0;
  uint64_t lastMono_ = 0;
  bool wasPlaying_ = false;
  Played ring_[kPending];
  uint32_t head_ = 0, count_ = 0;
  Counts counts_;
};

// ---------------------------------------------------------------------------
// The plays waiting, to the card (2.20.5; docs/METADATA.md 3.10).
// ---------------------------------------------------------------------------
// The Tracker's ring, oldest first, one play a call: the played file's size
// and FAT time asked of the card, the slot built (its start by the board's
// clock now), the journal's append. What the card fails isn't lost at once:
// the play stays at the head and is tried again, the journal's same slot
// (Journal::again(): its seq and its bytes, so a try that had landed isn't
// a second slot), after kRetryMs or when the card mounts again, kTries
// times in all; then it is dropped, counted. Nothing behind it is written
// meanwhile (the log's order is the plays'). A file that isn't on the card
// any more, NVS refusing the seq, a blocked log and no log drop the play at
// once, as they always did: another try changes nothing there.
class Outbox {
public:
  enum class Did : uint8_t {
    Nothing,  // none waiting
    Written,  // its slot appended and synced
    Dropped,  // no slot for it, for good
    Kept,     // the card failed it: at the head still, tried again when due()
  };
  enum class Why : uint8_t {
    None,
    Gone,     // the played file isn't on the card: dropped
    Off,      // no log this boot (the journal's Off): dropped
    Nvs,      // NVS didn't take the seq: dropped
    Blocked,  // no twin name free: dropped
    Stat,     // the card failed the question of the played file: kept (dropped at the last try)
    Append,   // the card failed the slot (the journal's Failed): kept (dropped at the last try)
  };
  struct Tried {
    Did did = Did::Nothing;
    Why why = Why::None;
    uint8_t tries = 0;    // of this play so far, this one counted (the card's failures and the last try)
    bool gaveUp = false;  // Dropped at its kTries-th card failure
    bool hasSlot = false; // `slot` is the play's (a seq was taken for it)
    Slot slot;
  };
  class Env {
  public:
    enum class File : uint8_t {
      There,   // *size and *fatTime are its
      Gone,    // the card says it isn't there
      Failed,  // the card failed the question: nothing is known of it
    };
    // The played file as the card has it now (f_stat, at the slot's write:
    // 2.20.5).
    virtual File stat(const char* path, uint32_t* size, uint32_t* fatTime) = 0;
    // The wall time of a play that began at `startMono` (Unix ms; 0 while
    // the clock is unset), and where the clock's time came from (the
    // slot's clock byte) in *clock.
    virtual uint64_t startedAt(uint64_t startMono, uint8_t* clock) = 0;
    // The board's monotonic ms (the wait before another try, the flush's
    // deadline).
    virtual uint64_t monoMs() = 0;
    // What a try came to, the play still at the head (the firmware's log
    // line).
    virtual void tried(const Tried& t, const Played& p) = 0;

  protected:
    ~Env() = default;
  };
  static constexpr uint8_t kTries = 3;
  static constexpr uint32_t kRetryMs = 5000;

  Outbox(Tracker& tracker, Journal& journal, Env& env) : tracker_(tracker), journal_(journal), env_(env) {}

  // The oldest play waiting, tried (the journal ready: its scan done).
  Tried writeOne();
  // No play is held back by a try the card failed, or its wait is over
  // (kRetryMs since that try; the card mounted again since: remounted()).
  bool due();
  // The card mounted again (after the journal's own remounted()): a play
  // held back is tried at once.
  void remounted() { remounted_ = true; }
  // The head play's tries so far (0: none failed).
  uint8_t tries() const { return tries_; }

  struct Flushed {
    enum class Stop : uint8_t {
      Done,  // every play waiting was tried
      Card,  // the card failed one: the rest not asked (each ask of a card that is going is seconds)
      Time,  // the deadline: a card too slow, or the boot's scan not done in it
    };
    uint32_t waiting = 0;  // when it began
    uint32_t written = 0;
    uint32_t dropped = 0;  // for good, each with its line
    uint32_t left = 0;     // NOT written: waiting still
    Stop stop = Stop::Done;
  };
  // The orderly end's (a restart, a power-off): the boot's scan finished
  // first, `scanSlots` a piece; then each play waiting tried once, in
  // order, whatever due() says. It ends at the first the card fails and at
  // `budgetMs`, so no restart waits on a card that has gone bad longer
  // than that and the call it was in.
  Flushed flush(uint32_t budgetMs, uint32_t scanSlots);

  struct Counts {
    uint32_t written = 0;
    uint32_t dropped = 0;  // for good, the lost ones among them
    uint32_t lost = 0;     // ... at their kTries-th card failure
    uint32_t retries = 0;  // tries after a first that the card failed
  };
  const Counts& counts() const { return counts_; }

private:
  Tried failed(Tried t, const Played& p);
  Tried done(Tried t, const Played& p);

  Tracker& tracker_;
  Journal& journal_;
  Env& env_;
  // The head play's: its card failures so far, when the last was, whether
  // a seq was taken for it (the journal has its slot: again()), the slot.
  uint8_t tries_ = 0;
  uint64_t triedAt_ = 0;
  bool remounted_ = false;
  bool taken_ = false;
  Slot slot_;
  Counts counts_;
};

// ---------------------------------------------------------------------------
// When the loop may touch the log (a slot's write, a piece of the boot's
// scan): the card's other reader is the decoder, and the write holds the
// loop, which draws a moving list's frames.
// ---------------------------------------------------------------------------
class WriteGate {
public:
  struct In {
    uint32_t nowMs = 0;
    bool playing = false;         // PlayState::Playing
    uint32_t ringMs = 0;          // the PCM ring's audio now
    uint32_t ringCapacityMs = 0;  // its size (0: unknown, no ring rule)
    uint32_t underruns = 0;       // the outputs' count, free-running
    uint32_t trackSeq = 0;        // the backend's starts and joins, free-running
    bool seeking = false;         // a start not taken up yet (a seek's, a skip's)
    bool decoderAtEnd = false;    // the decoder past the heard track's end (the next read ahead)
    uint32_t trackLeftMs = 0;     // what is left of the heard track (0: not known)
    // A list scrolls, flings or follows a finger, or a finger is on its A-Z
    // rail (ui::Ui::listMoving(): none while its page draws no frame; the
    // card worker waits for it too).
    bool listMoving = false;
  };
  enum class Wait : uint8_t {
    None,      // now
    Start,     // a start or a join: until kSettleMs after it (a seek's: its start taken up)
    TrackEnd,  // the gapless window: the heard track's last kEndMs, the decoder at its end
    Ring,      // playing, the ring below half
    Underrun,  // one in the last kUnderrunMs
    List,      // a list moves, playing or not (no limit: it rests, or the finger lifts)
  };
  // ESTIMATED (ScanScheduler's TrackChange and Ring rules; the device
  // check measures the write).
  static constexpr uint32_t kSettleMs = 2000;
  static constexpr uint32_t kEndMs = 5000;
  static constexpr uint32_t kUnderrunMs = 5000;
  // Every pass that may write: what the write waits for.
  Wait update(const In& in);
  static const char* waitName(Wait w);

private:
  bool primed_ = false;
  uint32_t trackSeq_ = 0, underruns_ = 0;
  uint32_t settleFrom_ = 0, underrunAt_ = 0;
  bool settling_ = false, underrun_ = false;
};

}  // namespace playlog
