// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/AutoDj.h"

#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CardContract.h"
#include "NvsLayout.h"
#include "Percentiles.h"
#include "app/AutoDjStore.h"
#include "app/Psram.h"
#include "storage/SectorDisk.h"

namespace cc = cardcontract;
namespace mpdj = cardcontract::mpdj;
using autodj::Began;
using autodj::Pass;

// The contract's: MPDJ major 2 since M5, the release that reaches Auto DJ
// from the UI (docs/AUTODJ-INDEX.md 7.4).
uint32_t autoDjReadMpdj() { return cc::devicetxt::current("").readMpdj; }

namespace {

uint64_t nowUs() { return static_cast<uint64_t>(esp_timer_get_time()); }
uint32_t randomSeed() { return esp_random(); }

autodj::Driver::Config driverConfig() {
  autodj::Driver::Config c;
  c.alloc = psramAlloc;
  c.release = psramFree;
  c.random = randomSeed;
  c.nowUs = nowUs;
  return c;
}

const char* modeName(autodj::Mode m) { return m == autodj::Mode::Locked ? "Stay on seed" : "Follow the vibe"; }

const char* phaseName(autodj::Join::Phase p) {
  using P = autodj::Join::Phase;
  switch (p) {
    case P::None: return "none";
    case P::Folders: return "hashing the library's folders";
    case P::Hash: return "hashing the library's paths";
    case P::Sort: return "sorting the runs";
    case P::Strings: return "STRS";
    case P::Filters: return "DJRF";
    case P::Paths: return "DJPH";
    case P::Done: return "done";
  }
  return "?";
}

const char* statusName(autodj::Join::Status s) {
  using S = autodj::Join::Status;
  switch (s) {
    case S::Idle: return "idle";
    case S::Working: return "under way";
    case S::Ready: return "ready";
    case S::Absent: return "ABSENT";
    case S::ReadError: return "a read failed";
    case S::Aborted: return "aborted (the library changed)";
    case S::NoMemory: return "NO PSRAM";
    case S::TooBig: return "a library over 65,534 tracks";
  }
  return "?";
}

const char* slotName(autodj::SlotResult r) {
  using R = autodj::SlotResult;
  switch (r) {
    case R::Ok: return "ok";
    case R::Suspect: return "its CRC failed (read again later)";
    case R::Damaged: return "DAMAGED (its CRC failed twice: skipped from now)";
    case R::Io: return "the read failed (the card?)";
    case R::Broken: return "BROKEN (a good CRC over a bad list: the index is absent until a remount)";
  }
  return "?";
}

unsigned pct(uint32_t part, uint32_t whole) {
  return whole ? static_cast<unsigned>((part * 100ull + whole / 2) / whole) : 0;
}

constexpr uint32_t kJoinRetryMs = 10000;  // a join whose read failed: again after (the card's, not the file's)
// A loop pass's slices of the join: as many as fit this (and one card read:
// Join::stepPass()). MEASURED 2026-10-13: one 1,000-hash slice a pass, 112
// slices, took 2.1 s idle and 7.2 s with a 320 kbps MP3 and the screen dark
// (20 ms of idle wait a pass); slices of a millisecond or two, several a
// pass, and main.cpp's short wait while it works (joinWorking()).
constexpr int64_t kJoinPassUs = 8000;

struct PassClock {
  int64_t endUs;
};
bool passTimeLeft(void* ctx) { return esp_timer_get_time() < static_cast<PassClock*>(ctx)->endUs; }

size_t psramLargest() { return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); }
size_t psramFreeBytes() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

// What the join's placement leaves free at least while its spacers hold the
// bottom of PSRAM's largest free block (AutoDjJoin.h), the few milliseconds
// of begin() (the 'joined' line's "in X ms"): another task's block
// meanwhile still fits (a track's start: the MP3 probe's 4 KB, a seek's
// 8 KB, the Opus packet buffer's 60 KB; the card worker's 4-16 KB buffers).
// The spacers over the whole block took the free to 10 KB at every join
// (MEASURED 2026-10-14). The maps taken after the spacers count (155 KB at
// 19,410 tracks), and so does the free outside the largest block; when
// they fall short, the rest of the margin is left above the blocks, which
// then sit that much under the block's top: so not more (128 KB put the
// filters 39 KB under it after a library update in test_autodj_join's
// model, where the runs had taken the holes). Not covered: the first Opus
// track's 125 KB of buffers (OpusGenerator, once a boot) starting in that
// window (the track would fail and be skipped: docs/AUTODJ.md 6.2).
constexpr size_t kJoinMarginBytes = 64 * 1024;

}  // namespace

AutoDj::AutoDj(Library& library, PlaybackController& player)
    : lib_(library), player_(player), dj_(driverConfig()), join_(psramAlloc, psramFree) {
  // The join's blocks where they take least of PSRAM's largest free block
  // (AutoDjJoin.h): in smaller free blocks that hold them, the rest at its
  // top.
  autodj::Join::Placement place;
  place.largest = psramLargest;
  place.freeBytes = psramFreeBytes;
  place.margin = kJoinMarginBytes;
  join_.setPlacement(place);
}

// ---------------------------------------------------------------------------
// The boot
// ---------------------------------------------------------------------------
void AutoDj::begin() {
  if (!dj_.begin()) Serial.println("[dj] no PSRAM for the engine's slot cache: picks by track id only");
  dj_.load(autodjstore::load());
  if (sectordisk::installed()) remountsSeen_ = sectordisk::guard().remounts;
  cardroot::Root* root = lib_.root();
  if (!lib_.records() || !root) {
    file_ = File::None;
    whyText_ = "no card records (the flash, or no PSRAM)";
    Serial.printf("[dj] index: none (%s): picks at random\n", whyText_);
  } else if (!root->autoDj.present) {
    file_ = File::None;
    whyText_ = root->present ? "the card's root names none" : "no transfer data on the card";
    Serial.printf("[dj] index: none (%s): picks at random\n", whyText_);
  } else {
    open(root->autoDj, "the boot");
  }
  if (dj_.settings().on) logSession(Began::Boot, autodj::kNoTrackId);
}

void AutoDj::absent(cc::Why why, const char* what) {
  file_ = File::Absent;
  why_ = why;
  whyText_ = what;
  Serial.printf("[dj] index %s: ABSENT (%s: %s): picks at random until a remount or the next boot\n", filePath_,
                what, cc::whyName(why));
}

void AutoDj::open(const cardroot::Root::AutoDj& e, const char* when) {
  closeFile();
  entry_ = e.entry;
  haveEntry_ = true;
  snprintf(filePath_, sizeof(filePath_), "%s", e.path);
  if (!cc::devicetxt::readsMajor(autoDjReadMpdj(), mpdj::kMajor2)) {
    file_ = File::NotRead;
    whyText_ = "this build doesn't read its major (read.mpdj)";
    Serial.printf("[dj] index %s: not read (%s): picks at random\n", filePath_, whyText_);
    return;
  }
  cardfat::FatFs* fs = lib_.fatfs();
  const int64_t t0 = esp_timer_get_time();
  tagstore::File* f = fs ? fs->open(filePath_, tagstore::Fs::Mode::Read) : nullptr;
  if (!f) {
    absent(cc::Why::Io, "the file the root names didn't open");
    return;
  }
  handle_ = static_cast<cardfat::FatFile*>(f);
  const cc::Why w = table_.open(*handle_);
  if (w != cc::Why::Ok) {
    closeFile();
    absent(w, "its header or directory failed a check");
    return;
  }
  if (!cc::msmf::companionMatches(entry_, table_.frame(), 0, table_.selectionSig())) {
    closeFile();
    absent(cc::Why::Comp, "not the file the root names (its generation, size, header CRC or selection)");
    return;
  }
  file_ = File::Open;
  why_ = cc::Why::Ok;
  whyText_ = "";
  block_ = Block::None;
  joinLogged_ = false;
  brokenSaid_ = false;
  dj_.engine().fileReopened();
  const uint8_t* sig = table_.selectionSig();
  Serial.printf("[dj] index %s (%s): %lu rows, %lu paths, k %lu (%s, head %lu, floor %lu), slots of %lu B, "
                "generation %lu, selection %02x%02x%02x%02x..., opened in %.1f ms; %s\n",
                filePath_, when, (unsigned long)table_.rowCount(), (unsigned long)table_.pathCount(),
                (unsigned long)table_.k(), table_.selKind() == mpdj::kSelTop ? "the top k" : "strided",
                (unsigned long)table_.selHead(), (unsigned long)table_.selFloor(), (unsigned long)table_.slotBytes(),
                (unsigned long)table_.frame().generation, sig[0], sig[1], sig[2], sig[3],
                (esp_timer_get_time() - t0) / 1000.0,
                dj_.settings().on ? "the join next" : "joined when Auto DJ is switched on");
}

void AutoDj::closeFile() {
  stopDryRun("the index closing");
  join_.release();  // (no join reads a closed file)
  if (handle_ && lib_.fatfs()) lib_.fatfs()->close(handle_);  // (after a remount its FIL is gone: freed anyway)
  handle_ = nullptr;
  if (file_ == File::Open) file_ = File::None;
}

bool AutoDj::usable() const {
  if (file_ != File::Open || dj_.engine().broken()) return false;
  const LibraryIndex* index = lib_.index();
  return !index || !index->ready() || index->trackCount() <= autodj::kMaxTracks;
}

void AutoDj::releaseJoin(const char* why) {
  stopDryRun(why);
  const size_t freed = join_.release();
  joinLogged_ = false;
  if (block_ == Block::Retry) block_ = Block::None;
  if (freed) Serial.printf("[dj] the join given back (%s): %u B of PSRAM\n", why, (unsigned)freed);
}

bool AutoDj::hasRow(uint32_t track) {
  if (joined()) {
    const autodj::Maps& m = *join_.maps();
    return track < m.tracks && m.trackToRow[track] != autodj::kNoRow;
  }
  if (file_ != File::Open || !handle_) return false;
  const LibraryIndex* index = lib_.index();
  if (!index || !index->ready() || track >= index->trackCount()) return false;
  uint64_t h = 0;
  if (!autodj::trackPathHash(*index, track, hashPath_, sizeof(hashPath_), &h)) return false;
  return table_.rowOf(h) != mpdj::kNoRow;
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------
autodj::Index AutoDj::index() {
  autodj::Index ix;
  if (joined()) {
    ix.maps = join_.maps();
    ix.slots = &reader_;
    ix.k = static_cast<uint16_t>(table_.k());
    ix.selHead = static_cast<uint16_t>(table_.selHead());
    ix.selFloor = static_cast<uint8_t>(table_.selFloor());
  }
  ix.joining = file_ == File::Open && !joined() && block_ == Block::None;
  return ix;
}

autodj::Env AutoDj::driverEnv(const Env& env) const {
  autodj::Env e;
  e.nowMs = env.nowMs;
  const LibraryIndex* index = lib_.index();
  e.libraryTracks = index && index->ready() ? index->trackCount() : 0;
  e.audioPlaying = env.audioPlaying;
  e.positionKnown = env.positionKnown;
  e.trackSeq = env.trackSeq;
  e.positionMs = env.positionMs;
  e.cardHeld = env.cardHeld;
  e.restartSet = env.restartSet;
  e.sleepEndOfQueue = env.sleepEndOfQueue;
  e.listMoving = env.listMoving;
  e.refilling = env.refilling;
  return e;
}

void AutoDj::step(const Env& env) {
  remount(env);
  stepJoin(env);
  const Pass p = dj_.step(player_, index(), driverEnv(env));
  report(p);
  stepJob(env);
}

bool AutoDj::takeUndoDropped() {
  const bool d = undoDropped_;
  undoDropped_ = false;
  return d;
}

// A FatFs remount (the card pulled and put back, maybe written on a PC
// meanwhile, or FatFs's own after a status check failed twice): the open
// file's FIL is gone with the old mount. The root read again into this
// Root (never Library::root(): the update step holds it, TagStore its
// identity); the same index: the file reopened, the maps and the session
// kept; another (or one now, or none now): closed, opened, joined again, a
// new session.
void AutoDj::remount(const Env& env) {
  if (!sectordisk::installed() || env.cardHeld) return;  // (cardHeld: another card, or a job: a restart comes)
  const uint32_t n = sectordisk::guard().remounts;
  if (n == remountsSeen_) return;
  remountsSeen_ = n;
  ++remounts_;
  if (!lib_.records() || !lib_.fatfs()) return;
  if (!root_) root_ = psramNew<cardroot::Root>();
  auto* scratch = static_cast<uint8_t*>(psramAlloc(1024));
  if (!root_ || !scratch) {
    psramFree(scratch);
    Serial.println("[dj] the card was mounted again: no PSRAM to read its root: the index is closed until the next "
                   "remount");
    closeFile();
    // Absent with its entry kept: the next remount with the same index
    // opens it again (the session kept); one with another index is new.
    file_ = haveEntry_ ? File::Absent : File::None;
    why_ = cc::Why::Io;
    whyText_ = "no PSRAM to read the root after a remount";
    return;
  }
  cardroot::read(*lib_.fatfs(), scratch, 1024, root_);
  psramFree(scratch);
  const cardroot::Root::AutoDj& e = root_->autoDj;
  // The same index: the same entry (generation, size, header CRC, the
  // selection), or none still.
  const bool same = e.present == haveEntry_ &&
                    (!e.present || (e.entry.generation == entry_.generation && e.entry.fileBytes == entry_.fileBytes &&
                                    e.entry.headerCrc == entry_.headerCrc &&
                                    std::memcmp(e.entry.key, entry_.key, sizeof(entry_.key)) == 0));
  if (same && file_ == File::Absent) {
    open(e, "after a remount");  // (a check a read failed may pass now; the session kept)
    return;
  }
  if (same && file_ != File::Open) return;  // (none still, or one this build doesn't read)
  if (same) {
    // The FIL again (the table's frame read again: one small read).
    const bool wasReady = joined();
    if (handle_) lib_.fatfs()->close(handle_);
    handle_ = nullptr;
    tagstore::File* f = lib_.fatfs()->open(filePath_, tagstore::Fs::Mode::Read);
    handle_ = static_cast<cardfat::FatFile*>(f);
    const cc::Why w = handle_ ? table_.open(*handle_) : cc::Why::Io;
    if (w != cc::Why::Ok || !cc::msmf::companionMatches(entry_, table_.frame(), 0, table_.selectionSig())) {
      join_.release();
      if (handle_) lib_.fatfs()->close(handle_);
      handle_ = nullptr;
      absent(w != cc::Why::Ok ? w : cc::Why::Comp, "it failed its checks after the remount");
      return;
    }
    dj_.engine().fileReopened();  // (the damaged ring: rows may read again; a broken mark goes)
    brokenSaid_ = false;
    if (!wasReady) {
      join_.release();  // (a join under way read the old FIL: again from its start)
      // (A retry the old FIL's failed read set waits no more: the join at
      // the next pass. A library too big, or no memory, still waits for
      // the next build.)
      if (block_ == Block::Retry) block_ = Block::None;
      joinLogged_ = false;
    }
    Serial.printf("[dj] the card was mounted again (the same index): the file reopened, %s\n",
                  wasReady            ? "the maps and the session kept"
                  : dj_.settings().on ? "the join again"
                                      : "the session kept (the index joins when Auto DJ is switched on)");
    return;
  }
  // Another index, one now, or none now.
  closeFile();
  haveEntry_ = false;
  if (!e.present) {
    file_ = File::None;
    whyText_ = root_->present ? "the card's root names none" : "no transfer data on the card";
    Serial.printf("[dj] the card was mounted again: no index now (%s): picks at random\n", whyText_);
  } else {
    open(e, "after a remount");
  }
  if (dj_.settings().on) {
    dj_.newSession(Began::IndexChanged, autodj::kNoTrackId, nullptr);
    logSession(Began::IndexChanged, autodj::kNoTrackId);
  }
}

// The join while Auto DJ is on: begun once the UI's first frame is up;
// its slices a pass up to kJoinPassUs and one card read, never while a
// list moves (30 fps: H7) or the card is held; none behind the update's
// fence (the index hidden).
void AutoDj::stepJoin(const Env& env) {
  if (file_ != File::Open || !dj_.settings().on) return;
  if (block_ == Block::Update) {
    // Given back to the update step's memory check: again after its Live
    // (live()), or now that it deferred or failed (over before a fence).
    LibraryUpdate* up = lib_.update();
    if (up && up->asked()) return;
    block_ = Block::None;
  }
  LibraryIndex* index = lib_.index();
  using S = autodj::Join::Status;
  const S s = join_.status();
  if (s == S::Idle || s == S::Aborted) {
    if (!index || !index->ready() || !env.uiStarted || env.cardHeld) return;
    if (block_ == Block::Build) return;
    if (block_ == Block::Retry && static_cast<int32_t>(env.nowMs - retryAtMs_) < 0) return;
    block_ = Block::None;
    beginJoin(index);
    return;
  }
  if (s != S::Working || env.listMoving || env.cardHeld) return;
  PassClock clock{esp_timer_get_time() + kJoinPassUs};
  join_.stepPass(index, passTimeLeft, &clock);
  ++joinPasses_;
  joinStatus();
}

void AutoDj::beginJoin(const LibraryIndex* index) {
  joinT0Us_ = esp_timer_get_time();
  joinLogged_ = false;
  joinPasses_ = 0;
  ++joins_;
  join_.begin(table_, index);
  // (Its placement asks PSRAM's largest free block a few times: the IDF
  // walks the heap for it.)
  beginMs_ = (esp_timer_get_time() - joinT0Us_) / 1000.0f;
  joinStatus();
}

void AutoDj::joinStatus() {
  using S = autodj::Join::Status;
  const S s = join_.status();
  if (s == S::Working || s == S::Idle) return;
  joinMs_ = (esp_timer_get_time() - joinT0Us_) / 1000.0f;
  const autodj::Join::Stats& st = join_.stats();
  switch (s) {
    case S::Ready: {
      if (joinLogged_) return;
      joinLogged_ = true;
      // The cached slots' runs were made from the maps before (a fence, a
      // remount): made again, a loop pass each (Engine::runDue()).
      dj_.engine().mapsChanged();
      const autodj::Maps& m = *join_.maps();
      // Where its blocks went (AutoDjJoin.h's placement): those that fit a
      // smaller free block there, the rest at the top of the largest.
      char where[160];
      if (st.topBytes == 0) {
        snprintf(where, sizeof(where), "all in smaller free blocks");
      } else if (st.placed) {
        snprintf(where, sizeof(where), "%u B at the top of the largest free one, %u of 3 kept and %lu transient "
                 "blocks in smaller ones", (unsigned)st.topBytes, (unsigned)st.keptElsewhere,
                 (unsigned long)st.runsElsewhere);
      } else {
        snprintf(where, sizeof(where), "%u B where the heap put them (no spacer), %u of 3 kept and %lu transient "
                 "blocks in smaller free ones", (unsigned)st.topBytes, (unsigned)st.keptElsewhere,
                 (unsigned long)st.runsElsewhere);
      }
      Serial.printf("[dj] joined: %lu of %lu tracks have neighbours (%lu rows of %lu; BPM %u %%, key %u %% of them) in "
                    "%.0f ms, %lu slices in %lu loop passes, %lu reads; %u B kept at %p, %u B at the peak (largest "
                    "block %u B; %s, in %.1f ms; PSRAM free at least %u B meanwhile; PSRAM's largest free block "
                    "now %u B)%s\n",
                    (unsigned long)m.mapped, (unsigned long)m.tracks, (unsigned long)st.rowsMapped,
                    (unsigned long)m.rows, pct(m.withBpm, m.mapped), pct(m.withKey, m.mapped), joinMs_,
                    (unsigned long)st.slices, (unsigned long)joinPasses_, (unsigned long)st.reads,
                    (unsigned)join_.heldBytes(), static_cast<const void*>(m.filters), (unsigned)st.peakBytes,
                    (unsigned)st.largestBlock, where, beginMs_, (unsigned)st.lowestFree, (unsigned)psramLargest(),
                    st.ambiguous ? "; some paths two tracks share left out" : "");
      return;
    }
    case S::Absent: {
      const cc::Why why = join_.why();
      join_.release();
      if (handle_ && lib_.fatfs()) lib_.fatfs()->close(handle_);
      handle_ = nullptr;
      absent(why, "the join found a section that fails its checks");
      return;
    }
    case S::ReadError:
      join_.release();
      block_ = Block::Retry;
      retryAtMs_ = static_cast<uint32_t>(millis()) + kJoinRetryMs;
      Serial.printf("[dj] the join: a read of the card failed: again in %lu s (picks at random meanwhile)\n",
                    (unsigned long)(kJoinRetryMs / 1000));
      return;
    case S::Aborted:
      Serial.println("[dj] the join: the library changed under it: again with the new one");
      return;
    case S::NoMemory:
    case S::TooBig:
      join_.release();
      block_ = Block::Build;
      Serial.printf("[dj] the join: %s: picks at random until the library is built again\n", statusName(s));
      return;
    default: return;
  }
}

// ---------------------------------------------------------------------------
// The fence (main.cpp's enterFence() and leaveFence())
// ---------------------------------------------------------------------------
void AutoDj::beforeFence() { dj_.fenceUp(player_.queue()); }

// A running Dn ends (its summary so far) before the maps go: its later
// picks would be by track id.
void AutoDj::stopDryRun(const char* by) {
  if (job_ != Job::DryRun) return;
  Serial.printf("[dj] Dn: stopped by %s (the maps go)\n", by);
  dryRunSummary(true);
}

size_t AutoDj::fence() {
  stopDryRun("the library update");
  const size_t freed = join_.release();
  joinLogged_ = false;
  if (block_ == Block::Build) block_ = Block::None;  // (a new build: try again)
  return freed;
}

size_t AutoDj::giveBack() {
  const size_t held = join_.heldBytes();
  if (!held) return 0;
  releaseJoin("the library update's memory check: the new track table's block");
  block_ = Block::Update;
  return held;
}

void AutoDj::live() {
  if (block_ == Block::Update) block_ = Block::None;
  dj_.remapped(player_.queue());
  Serial.printf("[dj] the library was rebuilt: %d DJ marks carried, the session kept; %s\n", dj_.marks().count(),
                dj_.settings().on ? "the join again" : "the index joins when Auto DJ is switched on");
}

// ---------------------------------------------------------------------------
// The log
// ---------------------------------------------------------------------------
const char* AutoDj::pathOf(uint32_t track) {
  if (track == autodj::kNoTrackId || !lib_.catalog().path(track, path_, sizeof(path_))) {
    snprintf(path_, sizeof(path_), "(none)");
  }
  return path_;
}

void AutoDj::logSession(Began why, uint32_t track) {
  newsWatch_.sessionBegan();  // (the UI's: a session's first pick, its random picks said once)
  Serial.printf("[dj] session %lu (%s): seed %08lx, %s%s\n", (unsigned long)dj_.sessions(), autodj::beganName(why),
                (unsigned long)dj_.sessionSeed(), track == autodj::kNoTrackId ? "no seed track" : "seeded by ",
                track == autodj::kNoTrackId ? "" : pathOf(track));
}

void AutoDj::logPick(const Pass& p, const char* how) {
  const autodj::Pick& k = p.pick;
  logPickLine(p, how);
  // A slot whose CRC failed again at this pick's read (read twice here, or
  // suspect since a read ahead): in the damaged ring from now, as the read
  // ahead's "DAMAGED" line says for its own. Once a row: a damaged row
  // isn't read again until the file is reopened.
  for (uint8_t i = 0; i < k.damaged; ++i) {
    Serial.printf("[dj] read at the pick, row %u: %s\n", (unsigned)k.damagedRow[i],
                  slotName(autodj::SlotResult::Damaged));
  }
}

void AutoDj::logPickLine(const Pass& p, const char* how) {
  const autodj::Pick& k = p.pick;
  if (!k.ok) {
    Serial.printf("[dj] %s: no track (%s): again in %lu s\n", how,
                  joined() ? "nothing to draw from" : "no index and no library track",
                  (unsigned long)(autodj::kRetryMs / 1000));
    return;
  }
  char row[24] = "no row";
  if (k.row != autodj::kNoRow) snprintf(row, sizeof(row), "row %u", (unsigned)k.row);
  Serial.printf("[dj] %s: %s (%s, level %u, class %u: cooldown %u, rank %u; %u candidates, %u gated, %u in its class; "
                "%u reads; %.2f ms, reads %.2f ms)%s%s%s%s%s%s%s%s%s%s%s\n",
                how, pathOf(k.track), row, (unsigned)k.level,
                (unsigned)(k.cooldownLevel * autodj::kRanks + k.musicRank),
                (unsigned)k.cooldownLevel, (unsigned)k.musicRank, (unsigned)k.candidates, (unsigned)k.gated,
                (unsigned)k.inClass, (unsigned)k.reads, p.pickUs / 1000.0, p.readUs / 1000.0,
                k.dry ? " [dry: the 50 closest]" : "",
                k.loosened ? " [loosened: the gate's rows all played lately: the 50 closest not]" : "",
                k.random && !k.noIndex ? " [random]" : "",
                k.noIndex ? " [by track id: no index]" : "", k.imputedTempo ? " [tempo drawn]" : "",
                k.imputedKey ? " [key drawn]" : "", k.tempoBreak ? " [tempo break]" : "",
                k.keyBreak ? " [key break]" : "", p.cued ? " [cued: the queue had finished; Play starts it]" : "",
                p.refused ? " [REFUSED by the queue: full of tracks not played yet, or no memory; again in 5 s]" : "",
                p.undoDropped ? " [the listener's undo went with it]" : "");
}

void AutoDj::report(const Pass& p) {
  if (p.began != Began::None) logSession(p.began, player_.queue().currentTrack());
  if (p.quickSkip) {
    // (Why: docs/AUTODJ.md 3.2; before the join its row is unknown, not
    // absent: "the index isn't joined yet".)
    Serial.printf("[dj] a quick skip: %s after %.1f s: %s\n", pathOf(p.leftTrack), p.leftMs / 1000.0,
                  autodj::skipWhyText(p.leftWhy));
  }
  if (p.removedPick) {
    Serial.printf("[dj] Auto DJ's pick taken out before it played: %s: no longer steers (still cooled)\n",
                  pathOf(p.removedTrack));
  }
  if (p.failed && p.failedPick && dj_.settings().on) {
    Serial.printf("[dj] Auto DJ's pick failed to play (%u in a row)%s\n", (unsigned)dj_.failurePause().inARow(),
                  p.paused ? ": Auto DJ PAUSED (it goes on once a track plays, or off and on)" : "");
  }
  if (p.picked) logPick(p, "pick");
  if (p.undoDropped) undoDropped_ = true;
  // The UI's toasts (autodjview::NewsWatch): a finished queue's cued pick;
  // the failure pause; a session's first pick random because its track
  // has no row (Stay on seed: that pick becomes the seed); picks by track
  // id for want of an index (once a switch on, or a boot).
  news_ |= newsWatch_.pass(p, dj_.settings().mode, dj_.settings().on);
  const bool broken = (p.picked && p.pick.broken) || (p.prefetched && p.prefetch == autodj::SlotResult::Broken);
  if (broken && !brokenSaid_) {
    brokenSaid_ = true;
    Serial.printf("[dj] index %s: a slot's list breaks a rule under a good CRC (the writer's bug): BROKEN, picks at "
                  "random until a remount or the next boot\n",
                  filePath_);
  }
  if (p.prefetched && p.prefetch != autodj::SlotResult::Ok) {
    Serial.printf("[dj] read ahead, row %u: %s\n", (unsigned)p.prefetchRow, slotName(p.prefetch));
  }
}

// ---------------------------------------------------------------------------
// The settings
// ---------------------------------------------------------------------------
void AutoDj::saveSettings(const char* what) {
  const bool saved = autodjstore::save(dj_.settings());
  Serial.printf("[dj] %s (%s)\n", what, saved ? "saved" : "NOT saved");
}

bool AutoDj::setOn(bool on) {
  if (!dj_.setOn(on)) {
    Serial.printf("[dj] Auto DJ %s already\n", on ? "on" : "off");
    return false;
  }
  saveSettings(!on                   ? "Auto DJ off: what it added stays and plays out"
               : file_ == File::Open ? "Auto DJ on: it adds a track when the queue's last one starts (a finished "
                                       "queue: one cued, paused); the index joins now"
                                     : "Auto DJ on: it adds a track when the queue's last one starts (a finished "
                                       "queue: one cued, paused)");
  if (on) logSession(Began::On, autodj::kNoTrackId);
  // No index it can use: its picks are random (the UI says so now, and
  // not again until the next switch on).
  if (on) news_ |= newsWatch_.switchedOn(usable());
  if (!on) releaseJoin("Auto DJ off");
  // Switched on again: a join that failed for memory (or a library too
  // big) is tried again, as the listener may have freed some since (a
  // library too big fails again at once).
  if (on && block_ == Block::Build) block_ = Block::None;
  return true;
}

bool AutoDj::setMode(autodj::Mode m) {
  if (!dj_.setMode(m)) return false;
  saveSettings(m == autodj::Mode::Locked ? "Anchor: Stay on seed (the seed: the playing track at the next pick)"
                                         : "Anchor: Follow the vibe");
  return true;
}

bool AutoDj::setFloor(uint8_t pct) {
  if (!dj_.setFloor(pct)) return false;
  char what[48];
  snprintf(what, sizeof(what), "Match strictness %u %%", (unsigned)pct);
  saveSettings(what);
  return true;
}

bool AutoDj::setMatch(bool tempo, bool key) {
  if (!dj_.setMatch(tempo, key)) return false;
  char what[64];
  snprintf(what, sizeof(what), "BPM continuity %s, Harmonic mixing %s", tempo ? "on" : "off", key ? "on" : "off");
  saveSettings(what);
  return true;
}

bool AutoDj::fromHere(uint32_t track) {
  bool switched = false;
  if (!dj_.playFrom(player_, track, Began::FromHere, index().maps, &switched)) {
    Serial.println("[dj] Auto DJ from here: the Play was refused (the library updating, or no memory)");
    return false;
  }
  if (switched) saveSettings("Auto DJ on");
  logSession(Began::FromHere, track);
  if (!hasRow(track)) {
    Serial.println("[dj] (that track has no neighbours in the index: the first pick is random, and becomes the seed)");
  }
  return true;
}

// The empty states' button (D4): a random track with neighbours, played,
// Auto DJ on. Joined, a row of the maps (Driver::surprise()); not (Auto DJ
// off: the join follows the switch), library tracks drawn and looked up by
// their path's hash in DJPH, so playback never waits for a join.
autodjview::Started AutoDj::start() {
  if (!usable()) {
    // (Gone since the button was drawn: a remount without it, a check
    // failed. The empty states draw Shuffle all again at the next pass.)
    Serial.printf("[dj] the Auto DJ button: no usable index (%s): nothing played\n",
                  whyText_ && whyText_[0] ? whyText_ : "found broken, or a library too big");
    return autodjview::Started::NoIndex;
  }
  uint32_t track = autodj::kNoTrackId;
  if (joined()) {
    const autodj::Pick s = dj_.surprise(index());
    if (s.ok) track = s.track;
  } else {
    const LibraryIndex* idx = lib_.index();
    const uint32_t tracks = idx && idx->ready() ? idx->trackCount() : 0;
    const int64_t t0 = esp_timer_get_time();
    uint8_t tries = 0;
    track = dj_.surpriseTrack(tracks, hasRowOf, this, &tries);
    Serial.printf("[dj] the Auto DJ button: %u library tracks looked up in the index by their path (%.1f ms): %s\n",
                  (unsigned)tries, (esp_timer_get_time() - t0) / 1000.0,
                  track != autodj::kNoTrackId ? "one has neighbours" : "none had neighbours");
  }
  if (track == autodj::kNoTrackId) {
    Serial.println("[dj] the Auto DJ button: no track with neighbours in the draws: nothing played");
    return autodjview::Started::NoTrack;
  }
  bool switched = false;
  if (!dj_.playFrom(player_, track, Began::Surprise, index().maps, &switched)) {
    Serial.println("[dj] the Auto DJ button: the Play was refused (the library updating, or no memory)");
    return autodjview::Started::Refused;
  }
  if (switched) saveSettings("Auto DJ on");
  logSession(Began::Surprise, track);
  return autodjview::Started::Played;
}

uint8_t AutoDj::takeNews() {
  const uint8_t n = news_;
  news_ = 0;
  return n;
}

void AutoDj::view(autodjview::State* v) const {
  using autodjview::Index;
  if (!v) return;
  const autodj::Settings& s = dj_.settings();
  *v = autodjview::State{};
  v->on = s.on;
  v->mode = s.mode;
  v->floorPct = s.floorPct;
  v->floorShown = file_ == File::Open ? autodjview::shownFloor(s.floorPct, table_.selFloor()) : s.floorPct;
  v->matchTempo = s.matchTempo;
  v->matchKey = s.matchKey;
  v->usable = usable();
  v->hold = s.on ? dj_.lastTrigger().hold : autodj::Hold::None;
  v->adds = newsWatch_.adds();
  const LibraryIndex* index = lib_.index();
  if (file_ == File::Absent && why_ == cc::Why::Io) {
    // A read failed (the file the root names didn't open, a card read
    // error, no PSRAM to read the root after a remount): not the index's
    // checks; a remount or the next boot tries again.
    v->index = Index::Unreadable;
  } else if (file_ == File::Absent || (file_ == File::Open && dj_.engine().broken())) {
    v->index = Index::Bad;
  } else if (file_ != File::Open) {
    v->index = Index::None;
  } else if (index && index->ready() && index->trackCount() > autodj::kMaxTracks) {
    v->index = Index::TooBig;
  } else if (joined()) {
    const autodj::Maps& m = *join_.maps();
    v->index = Index::Ready;
    v->mapped = m.mapped;
    v->tracks = m.tracks;
    v->bpmPct = static_cast<uint8_t>(pct(m.withBpm, m.mapped));
    v->keyPct = static_cast<uint8_t>(pct(m.withKey, m.mapped));
  } else if (block_ == Block::Build) {
    v->index = Index::NoMemory;
  } else if (!s.on) {
    v->index = Index::Found;
    v->rows = table_.rowCount();
  } else {
    v->index = Index::Reading;
  }
}

// ---------------------------------------------------------------------------
// The console
// ---------------------------------------------------------------------------
void AutoDj::command(const char* arg, const Env& env) {
  using K = autodj::Command::Kind;
  const autodj::Command c = autodj::parseCommand(arg);
  switch (c.kind) {
    case K::Status: status(env); return;
    case K::On:
    case K::Off: setOn(c.kind == K::On); return;
    case K::Mode:
      if (!setMode(c.n ? autodj::Mode::Locked : autodj::Mode::Rolling))
        Serial.printf("[dj] Anchor: %s already\n", modeName(dj_.settings().mode));
      return;
    case K::Floor:
      if (!setFloor(static_cast<uint8_t>(c.n))) Serial.printf("[dj] Match strictness %u %% already\n", (unsigned)c.n);
      return;
    case K::Tempo:
    case K::Key: {
      const autodj::Settings& s = dj_.settings();
      const bool tempo = c.kind == K::Tempo ? c.n != 0 : s.matchTempo;
      const bool key = c.kind == K::Key ? c.n != 0 : s.matchKey;
      if (!setMatch(tempo, key)) Serial.println("[dj] matching: as it was");
      return;
    }
    case K::Seed:
      dj_.setSeed(c.n);
      Serial.printf("[dj] the next session's seed: %08lx (Dr starts one now)\n", (unsigned long)c.n);
      return;
    case K::Renew:
      dj_.newSession(Began::Console, autodj::kNoTrackId, nullptr);
      logSession(Began::Console, autodj::kNoTrackId);
      return;
    case K::Pick: {
      if (player_.fenced()) {
        Serial.println("[dj] Dp: the library is updating (the queue is the build's): try again in a few seconds");
        return;
      }
      const Pass p = dj_.pickNow(player_, index(), driverEnv(env));
      logPick(p, "Dp");
      if (p.undoDropped) undoDropped_ = true;
      news_ |= newsWatch_.pass(p, dj_.settings().mode, dj_.settings().on);  // (its append: no badge flash)
      Serial.print("[dj] Dp: the anchors (rows, oldest first):");
      const autodj::Engine& e = dj_.engine();
      for (uint8_t i = 0; i < e.anchors(); ++i) Serial.printf(" %u", (unsigned)e.anchor(i));
      Serial.printf("; the pin %s\n", e.pin() == autodj::kNoRow ? "none" : String(e.pin()).c_str());
      return;
    }
    case K::DryRun: {
      if (job_ != Job::None) {
        Serial.println("[dj] Dn: a long command runs (Dx stops it)");
        return;
      }
      if (!dry_) dry_ = psramNew<autodj::DryRun>();
      if (!dry_ || !dry_->begin(dj_.engine(), c.n, player_.queue().currentTrack(), psramAlloc, psramFree)) {
        Serial.println("[dj] Dn: no PSRAM for the session's copy");
        return;
      }
      job_ = Job::DryRun;
      Serial.printf("[dj] Dn: %lu dry-run picks on a copy of the session (%s, %u %%, BPM %s, key %s; %s), one a loop "
                    "pass; the live session untouched\n",
                    (unsigned long)c.n, modeName(dj_.settings().mode), (unsigned)dj_.settings().floorPct,
                    dj_.settings().matchTempo ? "on" : "off", dj_.settings().matchKey ? "on" : "off",
                    joined() ? "the index" : "no index: by track id");
      return;
    }
    case K::Join:
      if (file_ != File::Open) {
        Serial.printf("[dj] Dj: no index open (%s)\n", whyText_);
        return;
      }
      if (!dj_.settings().on) {
        Serial.println("[dj] Dj: Auto DJ is off: the index joins only while it is on (D1 joins it)");
        return;
      }
      stopDryRun("Dj");
      join_.release();
      block_ = Block::None;
      joinLogged_ = false;
      Serial.println("[dj] Dj: the join again from its start (its line follows when it is done)");
      return;
    case K::Reopen: {
      const cardroot::Root* root = root_ ? root_ : lib_.root();  // (a remount's, else the boot's)
      if (!root || !root->autoDj.present) {
        Serial.println("[dj] Dc: the card's root names no index");
        return;
      }
      Serial.println("[dj] Dc: the file closed and opened again, as a remount that found another index (a new "
                     "session)");
      stopDryRun("Dc");
      closeFile();
      haveEntry_ = false;
      open(root->autoDj, "Dc");
      dj_.newSession(Began::IndexChanged, autodj::kNoTrackId, nullptr);
      logSession(Began::IndexChanged, autodj::kNoTrackId);
      return;
    }
    case K::Bench: {
      if (job_ != Job::None) {
        Serial.println("[dj] Db: a long command runs (Dx stops it)");
        return;
      }
      if (file_ != File::Open || !handle_) {
        Serial.printf("[dj] Db: no index open (%s)\n", whyText_);
        return;
      }
      benchUs_ = static_cast<float*>(psramAlloc(c.n * sizeof(float)));
      benchBuf_ = static_cast<uint8_t*>(psramAlloc(mpdj::kLargeSlot));
      if (!benchUs_ || !benchBuf_) {
        psramFree(benchUs_);
        psramFree(benchBuf_);
        benchUs_ = nullptr;
        benchBuf_ = nullptr;
        Serial.println("[dj] Db: no PSRAM");
        return;
      }
      job_ = Job::Bench;
      benchWant_ = c.n;
      benchDone_ = benchFails_ = 0;
      benchCheckUs_ = 0;
      benchCheckMax_ = 0;
      Serial.printf("[dj] Db: %lu slot reads of random rows (%lu B each), one a loop pass, as the queue plays\n",
                    (unsigned long)c.n, (unsigned long)table_.slotBytes());
      return;
    }
    case K::FromHere: {
      if (player_.fenced()) {
        Serial.println("[dj] Dh: the library is updating: try again in a few seconds");
        return;
      }
      const LibraryIndex* index = lib_.index();
      const uint32_t track = c.n == autodj::Command::kCurrent ? player_.queue().currentTrack() : c.n;
      if (track == QueueModel::kNone || !index || !index->ready() || track >= index->trackCount()) {
        Serial.println("[dj] Dh: no such library track (Dh: the current entry's; Dh<id>: a library track id)");
        return;
      }
      fromHere(track);
      return;
    }
    case K::Surprise:
      if (player_.fenced()) {
        Serial.println("[dj] De: the library is updating: try again in a few seconds");
        return;
      }
      start();
      return;
    case K::Stop:
      if (job_ == Job::DryRun) {
        dryRunSummary(true);
      } else if (job_ == Job::Bench) {
        benchSummary(true);
      } else {
        Serial.println("[dj] Dx: nothing runs");
      }
      return;
    case K::Help:
      Serial.printf("[dj] %s\n", autodj::commandHelp());
      return;
    case K::Bad:
      Serial.printf("[dj] D%s: not a D command. %s\n", arg ? arg : "", autodj::commandHelp());
      return;
  }
}

void AutoDj::stepJob(const Env& env) {
  // Not while a list moves (a read behind the decoder costs a frame: H7),
  // nor while the card is held (another card, a restart: the FIL is the
  // old mount's): Dn and Db go on at the next pass after.
  if (job_ == Job::None || env.listMoving || env.cardHeld) return;
  if (job_ == Job::DryRun) {
    if (player_.fenced()) return;
    if (!dry_->step(dj_.settings(), index(), driverEnv(env).libraryTracks, nowUs)) dryRunSummary(false);
    return;
  }
  if (job_ != Job::Bench) return;
  if (file_ != File::Open || !handle_) {
    Serial.println("[dj] Db: the index closed under it");
    benchSummary(true);
    return;
  }
  const uint32_t rows = table_.rowCount();
  const uint32_t row = rows ? esp_random() % rows : 0;
  const uint32_t bytes = table_.slotBytes();
  const int64_t t0 = esp_timer_get_time();
  const bool read = handle_->read(table_.slotsOffset() + row * bytes, benchBuf_, bytes);
  const int64_t t1 = esp_timer_get_time();
  uint32_t count = 0;
  const bool ok =
      read && mpdj::checkSlot(benchBuf_, bytes, table_.k(), row, rows, nullptr, &count) == mpdj::SlotRead::Ok;
  const uint32_t checkUs = static_cast<uint32_t>(esp_timer_get_time() - t1);
  benchUs_[benchDone_++] = static_cast<float>(t1 - t0);
  benchCheckUs_ += checkUs;
  if (checkUs > benchCheckMax_) benchCheckMax_ = checkUs;
  benchFails_ += ok ? 0 : 1;
  if (benchDone_ == benchWant_) benchSummary(false);
}

void AutoDj::dryRunSummary(bool stopped) {
  if (!dry_ || !dry_->begun()) {
    job_ = Job::None;
    return;
  }
  const autodj::DryRun::Result& r = dry_->result();
  const uint32_t n = dry_->done();
  float* t = dry_->times();
  if (stopped) std::sort(t, t + n);
  const Percentiles q = Percentiles::of(t, n);  // (sorted already: sorts again, cheaply)
  Serial.printf("[dj] Dn%s: %lu picks (%lu gave no track): %lu dry, %lu loosened, %lu random (%lu by id); repeats %lu, "
                "artists %lu; tempo breaks %lu, key breaks %lu; drawn: tempo %lu, key %lu\n",
                stopped ? " (stopped)" : "", (unsigned long)r.picks, (unsigned long)r.failed, (unsigned long)r.dry,
                (unsigned long)r.loosened, (unsigned long)r.random, (unsigned long)r.noIndex, (unsigned long)r.repeats,
                (unsigned long)r.artists, (unsigned long)r.tempoBreaks, (unsigned long)r.keyBreaks,
                (unsigned long)r.imputedTempo, (unsigned long)r.imputedKey);
  Serial.printf("[dj] Dn: reads at a pick 0/1/2: %lu/%lu/%lu, read ahead %lu, anchors left out %lu; a pick's time "
                "p50 %.0f us, p90 %.0f us, max %.0f us\n",
                (unsigned long)r.readsAtPick[0], (unsigned long)r.readsAtPick[1], (unsigned long)r.readsAtPick[2],
                (unsigned long)r.prefetchReads, (unsigned long)r.leftOut, q.p50, q.p90, q.max);
  Serial.print("[dj] Dn: classes (cooldown x 8 + rank: picks):");
  for (uint32_t c = 0; c < autodj::kClasses; ++c)
    if (r.classes[c]) Serial.printf(" %lu: %lu", (unsigned long)c, (unsigned long)r.classes[c]);
  Serial.println();
  dry_->end();
  job_ = Job::None;
}

void AutoDj::benchSummary(bool stopped) {
  const uint32_t n = benchDone_;
  uint32_t buckets[7] = {};  // <1, <2, <5, <10, <20, <50, >= 50 ms
  for (uint32_t i = 0; i < n; ++i) {
    const float ms = benchUs_[i] / 1000.0f;
    buckets[ms < 1 ? 0 : ms < 2 ? 1 : ms < 5 ? 2 : ms < 10 ? 3 : ms < 20 ? 4 : ms < 50 ? 5 : 6]++;
  }
  const Percentiles q = Percentiles::of(benchUs_, n);
  Serial.printf("[dj] Db%s: %lu reads (%lu failed or failed the check): p50 %.0f us, p90 %.0f us, max %.0f us; "
                "<1/<2/<5/<10/<20/<50/50+ ms: %lu/%lu/%lu/%lu/%lu/%lu/%lu; the check (CRC and list) %.0f us mean, "
                "%lu us max\n",
                stopped ? " (stopped)" : "", (unsigned long)n, (unsigned long)benchFails_, q.p50, q.p90, q.max,
                (unsigned long)buckets[0], (unsigned long)buckets[1], (unsigned long)buckets[2],
                (unsigned long)buckets[3], (unsigned long)buckets[4], (unsigned long)buckets[5],
                (unsigned long)buckets[6], n ? static_cast<double>(benchCheckUs_) / n : 0.0,
                (unsigned long)benchCheckMax_);
  psramFree(benchUs_);
  psramFree(benchBuf_);
  benchUs_ = nullptr;
  benchBuf_ = nullptr;
  job_ = Job::None;
}

void AutoDj::status(const Env& env) {
  const autodj::Settings& s = dj_.settings();
  const autodj::TriggerOut& t = dj_.lastTrigger();
  Serial.printf("[dj] Auto DJ %s; %s, strictness %u %%, BPM continuity %s, Harmonic mixing %s; now: %s%s%s\n",
                s.on ? "ON" : "off", modeName(s.mode), (unsigned)s.floorPct, s.matchTempo ? "on" : "off",
                s.matchKey ? "on" : "off", autodj::waitName(t.wait), t.owed ? " (owed: the update step waits)" : "",
                dj_.retryWaits(env.nowMs) ? "; a refused pick waits to be tried again" : "");
  // The index.
  switch (file_) {
    case File::None: Serial.printf("[dj] index: none (%s): picks at random\n", whyText_); break;
    case File::NotRead: Serial.printf("[dj] index %s: not read (%s)\n", filePath_, whyText_); break;
    case File::Absent:
      Serial.printf("[dj] index %s: ABSENT (%s: %s): picks at random\n", filePath_, whyText_, cc::whyName(why_));
      break;
    case File::Open: {
      const uint8_t* sig = table_.selectionSig();
      Serial.printf("[dj] index %s: generation %lu, selection %02x%02x%02x%02x..., %lu rows, k %lu, head %lu, floor "
                    "%lu (the gate: level %u)%s\n",
                    filePath_, (unsigned long)table_.frame().generation, sig[0], sig[1], sig[2], sig[3],
                    (unsigned long)table_.rowCount(), (unsigned long)table_.k(), (unsigned long)table_.selHead(),
                    (unsigned long)table_.selFloor(), (unsigned)autodj::floorLevel(s.floorPct, table_.selFloor()),
                    dj_.engine().broken() ? "; BROKEN (a bad list under a good CRC): random picks until a remount"
                                          : "");
      break;
    }
  }
  // The join.
  const autodj::Join::Stats& js = join_.stats();
  if (joined()) {
    const autodj::Maps& m = *join_.maps();
    Serial.printf("[dj] join: ready: %lu of %lu tracks have neighbours (%lu without a row), BPM %u %%, key %u %%; "
                  "%.0f ms, %lu slices, peak %u B (largest block %u B); joins %lu, remounts seen %lu\n",
                  (unsigned long)m.mapped, (unsigned long)m.tracks, (unsigned long)(m.tracks - m.mapped),
                  pct(m.withBpm, m.mapped), pct(m.withKey, m.mapped), joinMs_, (unsigned long)js.slices,
                  (unsigned)js.peakBytes, (unsigned)js.largestBlock, (unsigned long)joins_, (unsigned long)remounts_);
  } else if (file_ == File::Open && !s.on && join_.status() == autodj::Join::Status::Idle) {
    Serial.println("[dj] join: none (Auto DJ off: the index joins when it is switched on); 0 B held");
  } else if (file_ == File::Open) {
    Serial.printf("[dj] join: %s, %s (%lu slices in %lu loop passes, %lu reads, %lu tracks hashed, %lu paths read)%s\n",
                  statusName(join_.status()), phaseName(join_.phase()), (unsigned long)js.slices,
                  (unsigned long)joinPasses_, (unsigned long)js.reads, (unsigned long)js.hashed,
                  (unsigned long)js.paths,
                  block_ == Block::Build    ? "; waits for the next library build"
                  : block_ == Block::Retry  ? "; tried again soon"
                  : block_ == Block::Update ? "; given back to the library update (again after its Live)"
                                            : "");
  }
  // The session.
  const autodj::Engine& e = dj_.engine();
  Serial.printf("[dj] session %lu (%s): seed %08lx%s; the anchors (rows):", (unsigned long)dj_.sessions(),
                autodj::beganName(dj_.sessionBegan()), (unsigned long)dj_.sessionSeed(),
                dj_.seedSet() ? " (the next one's is set)" : "");
  for (uint8_t i = 0; i < e.anchors(); ++i) Serial.printf(" %u", (unsigned)e.anchor(i));
  Serial.printf("; the pin %s; key anchor %u; cooled %u rows, %u artists, %u by id; damaged %u; DJ marks %d; the "
                "queue's epoch %lu\n",
                e.pin() == autodj::kNoRow ? "none" : String(e.pin()).c_str(), (unsigned)e.keyAnchor(),
                (unsigned)e.cooledRows(), (unsigned)e.cooledArtists(), (unsigned)e.cooledIds(),
                (unsigned)e.damagedRows(), dj_.marks().count(), (unsigned long)player_.queue().playEpoch());
  // The counters.
  const autodj::Stats& st = e.stats();
  Serial.printf("[dj] picks %lu (none %lu): reads at a pick 0/1/2 %lu/%lu/%lu, read ahead %lu, left out %lu; dry %lu, "
                "loosened %lu, random %lu (by id %lu); drawn tempo %lu, key %lu; damaged %lu, read failures %lu, "
                "broken %lu; quick skips %lu, picks taken out %lu; holds %lu, failure pause %u in a row%s; refused "
                "%lu, undos taken %lu; waited for a refill %lu\n",
                (unsigned long)st.picks, (unsigned long)st.failed, (unsigned long)st.readsAtPick[0],
                (unsigned long)st.readsAtPick[1], (unsigned long)st.readsAtPick[2], (unsigned long)st.prefetchReads,
                (unsigned long)st.leftOut, (unsigned long)st.dry, (unsigned long)st.loosened, (unsigned long)st.random,
                (unsigned long)st.noIndex,
                (unsigned long)st.imputedTempo, (unsigned long)st.imputedKey, (unsigned long)st.damaged,
                (unsigned long)st.ioSkips, (unsigned long)st.broken, (unsigned long)st.quickSkips,
                (unsigned long)dj_.removedPicks(), (unsigned long)dj_.holds(), (unsigned)dj_.failurePause().inARow(),
                dj_.failurePause().paused() ? " (PAUSED)" : "", (unsigned long)dj_.refusals(),
                (unsigned long)dj_.undoDrops(), (unsigned long)dj_.refillWaits());
  Serial.printf("[dj] per mode: Follow the vibe %lu picks, tempo breaks %lu, key breaks %lu; Stay on seed %lu picks, "
                "tempo breaks %lu, key breaks %lu\n",
                (unsigned long)st.modePicks[0], (unsigned long)st.tempoBreaks[0], (unsigned long)st.keyBreaks[0],
                (unsigned long)st.modePicks[1], (unsigned long)st.tempoBreaks[1], (unsigned long)st.keyBreaks[1]);
  Serial.print("[dj] classes (cooldown x 8 + rank: picks):");
  for (uint32_t c = 0; c < autodj::kClasses; ++c)
    if (st.classes[c]) Serial.printf(" %lu: %lu", (unsigned long)c, (unsigned long)st.classes[c]);
  Serial.println();
  Pass last;
  if (dj_.lastPick(&last)) {
    Serial.printf("[dj] the last pick: %u reads, %.2f ms (reads %.2f ms, the rest %.2f ms)\n",
                  (unsigned)last.pick.reads,
                  last.pickUs / 1000.0, last.readUs / 1000.0, (last.pickUs - last.readUs) / 1000.0);
  }
  // Memory.
  Serial.printf("[dj] memory: the engine %u B, the join %u B (PSRAM); this %u B, its root %u B, the file %u B; PSRAM "
                "free %u B (largest %u B)%s\n",
                (unsigned)e.heldBytes(), (unsigned)join_.heldBytes(), (unsigned)sizeof(AutoDj),
                root_ ? (unsigned)sizeof(cardroot::Root) : 0u, handle_ ? (unsigned)sizeof(cardfat::FatFile) : 0u,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                job_ == Job::DryRun ? "; Dn runs" : job_ == Job::Bench ? "; Db runs" : "");
}
