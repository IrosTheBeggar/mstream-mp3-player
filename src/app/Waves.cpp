// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/Waves.h"

#include <Arduino.h>
#include <esp_timer.h>

#include <atomic>
#include <cstdio>

#include "CardContract.h"
#include "TagText.h"
#include "app/CardWorker.h"
#include "app/Library.h"
#include "app/Psram.h"
#include "storage/CardFat.h"
#include "storage/SectorDisk.h"

namespace cc = cardcontract;
namespace mpwf = cardcontract::mpwf;

// The contract's: major 1 (docs/WAVEFORM-INDEX.md 7.4).
uint32_t wavesReadMpwf() { return cc::devicetxt::current("").readMpwf; }

namespace {

constexpr uint32_t kRootScratch = 1024;  // the root's streams at a remount (cardroot::read())

// What the log's ABSENT line says of WaveState::why().
const char* absentWhat(cc::Why why) {
  switch (why) {
    case cc::Why::Io: return "the file the root names isn't on the card";
    case cc::Why::Comp: return "not the file the root names (its generation, size or header CRC)";
    case cc::Why::WvSlotOrder: return "a good CRC over a hash that breaks the sort rule";
    default: return "its header or directory failed a check";
  }
}

void printAbsent(const char* path, cc::Why why) {
  Serial.printf("[wave] index %s: ABSENT (%s: %s): plain lines until a remount or the next boot\n", path,
                absentWhat(why), cc::whyName(why));
}

// A path hash as the log has it: "(none)" for 0.
void hashText(uint64_t h, char* buf, size_t size) {
  if (!h) {
    snprintf(buf, size, "(none)");
    return;
  }
  snprintf(buf, size, "%08lx%08lx", (unsigned long)(h >> 32), (unsigned long)(h & 0xFFFFFFFFu));
}

}  // namespace

// The one job in flight: filled by the loop (start()), worked on by the worker,
// read back by the loop (loop()). The state word hands it over: only loads and
// stores (it is in PSRAM, where the ESP32 can't do atomic read-modify-writes).
// The answer's generation is WaveState's: it knows which remount the job began
// under, and start() never touches a job under way.
struct Waves::Job {
  std::atomic<uint8_t> state{Idle};
  // In.
  uint64_t hash = 0;
  cc::msmf::Comp entry;
  char path[sizeof(cardroot::Root::Waves::path)] = "";
  // What the read works in (wave::readShape(): none of it on the worker's stack).
  // The file too, opened in here for each job (as FatCard's Work): its 4.1 KB is
  // asked for once, with the job, not at each track change, when the decoder asks
  // for its own. An open can't fail for want of it.
  cardfat::FatFile file;
  mpwf::Table table;
  mpwf::Slot slot;
  // Out.
  wave::ReadResult result;
};

// ---- the worker's side ----

void Waves::stepEntry(void* job) {
  Job& j = *static_cast<Job*>(job);
  j.state.store(Working);
  const int64_t t0 = esp_timer_get_time();
  const FRESULT opened = j.file.openRead(j.path);
  if (opened == FR_OK) {
    wave::readShape(j.file, j.entry, j.hash, &j.table, &j.slot, &j.result);
    j.file.closeRead();
  } else {
    // No Source to ask. Only FatFs's word that it isn't there turns the file off:
    // a sector read that failed in the open sets nothing that makes a remount, and
    // would leave plain lines for the rest of the session.
    wave::openFailed(opened == FR_NO_FILE || opened == FR_NO_PATH, &j.result);
  }
  j.result.jobMs = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
  j.state.store(Done);
}

// ---- the loop's side ----

bool Waves::take(const cardroot::Root& root) {
  if (!root.waves.present) {
    whyNone_ = root.present ? "the card's root names none" : "no transfer data on the card";
    return false;
  }
  if (!cc::devicetxt::readsMajor(wavesReadMpwf(), mpwf::kMajor1)) {
    whyNone_ = "this build doesn't read its major (read.mpwf)";
    return false;
  }
  if (!job_) job_ = psramNew<Job>();
  if (!job_) {
    whyNone_ = "no PSRAM for its job";
    return false;
  }
  entry_ = root.waves.entry;
  snprintf(path_, sizeof(path_), "%s", root.waves.path);
  return true;
}

void Waves::begin() {
  if (sectordisk::installed()) remountsSeen_ = sectordisk::guard().remounts;
  const cardroot::Root* root = library_.records() ? library_.root() : nullptr;
  if (root && root->readFailed()) {
    state_.begin(false);
    rootFailed();
    return;
  }
  const bool named = root && take(*root);
  state_.begin(named);
  if (named) {
    Serial.printf("[wave] index %s: named by the root (generation %lu)\n", path_, (unsigned long)entry_.generation);
  } else {
    Serial.printf("[wave] index: none (%s)\n", whyNone_);
  }
}

bool Waves::start(CardWorker& worker, uint8_t priority, uint32_t nowMs) {
  if (!wantsRead(nowMs)) return false;
  Job& j = *job_;
  j.hash = state_.wanted();
  j.entry = entry_;
  snprintf(j.path, sizeof(j.path), "%s", path_);
  j.state.store(Queued);
  if (!worker.start(CardWorker::Job::Wave, priority, stepEntry, job_, nowMs)) {
    j.state.store(Idle);
    state_.refused(nowMs);
    return false;
  }
  state_.started();
  return true;
}

void Waves::loop(uint32_t nowMs) {
  if (!job_ || job_->state.load() != Done) return;
  const WaveState::Finish f = state_.finished(job_->result, nowMs);
  job_->state.store(Idle);
  if (f == WaveState::Finish::Absent) printAbsent(path_, state_.why());
  if (f == WaveState::Finish::GaveUp) {
    Serial.printf("[wave] index %s: a read failed, and again: no shape for this track until it changes\n", path_);
  }
}

// A FatFs remount (the card pulled and put back, maybe written on a PC
// meanwhile, or FatFs's own after a status check failed twice). Nothing of the
// file is open, so there is nothing to reopen: the root read again says which
// file it is now, and whatever was known of the last one goes. A job under way
// isn't touched: its answer comes in and is dropped (WaveState::generation()).
// Soon after a read of the root the card failed, a remount is that read's echo
// (autodj::OpenTries). And the same again for that read, when its wait is over.
void Waves::remount(bool cardHeld, bool retryHeld) {
  if (cardHeld) return;
  const uint32_t nowMs = static_cast<uint32_t>(millis());
  bool moved = false;
  if (sectordisk::installed()) {
    const uint32_t n = sectordisk::guard().remounts;
    moved = n != remountsSeen_;
    if (moved) {
      remountsSeen_ = n;
      ++remounts_;
    }
  }
  const bool news = moved && rootTries_.remounted(nowMs);
  const bool due = !moved && rootAgain_ && !retryHeld && rootTries_.due(nowMs);
  if (!news && !due) return;
  cardroot::Root* root = library_.records() && library_.fatfs() ? psramNew<cardroot::Root>() : nullptr;
  auto* scratch = root ? static_cast<uint8_t*>(psramAlloc(kRootScratch)) : nullptr;
  bool named = false, failed = false;
  if (scratch) {
    cardroot::read(*library_.fatfs(), scratch, kRootScratch, root);
    failed = root->readFailed();
    named = !failed && take(*root);
  } else {
    whyNone_ = library_.records() ? "no PSRAM to read the root" : "no card records";
  }
  psramFree(scratch);
  psramDelete(root);
  if (failed) {
    library_.fatfs()->echoNow();
    if (news) state_.remounted(false);  // (whatever was known of the last one goes, as below)
    rootFailed();
    return;
  }
  rootAgain_ = false;
  rootTries_.reset();
  state_.remounted(named);
  const char* how = news ? "the card was mounted again" : "the root tried again";
  if (named) {
    Serial.printf("[wave] %s: index %s: named by the root (generation %lu)\n", how, path_,
                  (unsigned long)entry_.generation);
  } else {
    Serial.printf("[wave] %s: no index now (%s)\n", how, whyNone_);
  }
}

// None on a timer when the boot failed it and the card jobs run, as Auto DJ's.
void Waves::rootFailed() {
  rootAgain_ = rootTries_.failed(autodj::OpenTry::Card, static_cast<uint32_t>(millis())) && !library_.jobsOnUnreadRoot();
  if (rootAgain_) {
    whyNone_ = "the card failed reading its root: tried again soon";
    Serial.printf("[wave] the card's root: a read failed: again in %lu s, %u of %u\n",
                  (unsigned long)(rootTries_.lastWaitMs() / 1000), (unsigned)rootTries_.used(),
                  (unsigned)autodj::OpenTries::kAgain);
  } else {
    whyNone_ = "the card failed every read of its root";
    Serial.println("[wave] the card's root: every read failed: plain lines until a remount or the next boot");
  }
}

// ---- the console: gw?, gw=, gw#, gw- ----

void Waves::command(const char* arg) {
  switch (arg[0]) {
    case '=': {
      uint8_t heights[wave::kColumns];
      if (!tagtext::digitsIn(arg + 1, heights, sizeof(heights))) {
        Serial.printf("[wave] gw=: %d digits 0-9, a height a column; nothing changed\n", wave::kColumns);
        return;
      }
      state_.inject(heights);
      Serial.println("[wave] gw=: a test shape, with no card, on whatever plays until gw-");
      break;
    }
    case '#': {
      uint8_t bars[mpwf::kBarCount];
      uint8_t heights[wave::kColumns];
      if (!tagtext::hexIn(arg + 1, bars, sizeof(bars))) {
        Serial.printf("[wave] gw#: %u hex digits, 2 a bar; nothing changed\n", (unsigned)(2 * mpwf::kBarCount));
        return;
      }
      wave::reduce(bars, heights);
      state_.inject(heights);
      Serial.println("[wave] gw#: a test shape from these bars, with no card, on whatever plays until gw-");
      break;
    }
    case '-':
      state_.dropInjected();
      Serial.println("[wave] gw-: no test shape");
      break;
    default: break;  // gw?
  }
  printState();
}

void Waves::printState() const {
  // The file.
  if (state_.file() == WaveState::File::Absent) {
    printAbsent(path_, state_.why());
  } else if (state_.file() == WaveState::File::Named) {
    Serial.printf("[wave] index %s: named by the root (generation %lu)\n", path_, (unsigned long)entry_.generation);
  } else {
    Serial.printf("[wave] index: none (%s)\n", whyNone_);
  }
  // The track, and what is known of it.
  char wanted[20], held[20];
  hashText(state_.wanted(), wanted, sizeof(wanted));
  hashText(state_.heldHash(), held, sizeof(held));
  Serial.printf("[wave] wanted %s, a read %s, %lu failed for it; the record: %s, %s\n", wanted,
                state_.reading() ? "under way" : "not under way", (unsigned long)state_.failedReads(), held,
                !state_.heldHash() ? "empty" : state_.heldShape() ? "a shape" : "no shape (no data, or a damaged slot)");
  if (const wave::Shape* shape = state_.shape()) {
    char digits[wave::kColumns + 1];
    for (int i = 0; i < wave::kColumns; ++i) digits[i] = static_cast<char>('0' + shape->heights[i]);
    digits[wave::kColumns] = 0;
    Serial.printf("[wave] it shows %s: %lu ms, heights %s\n", state_.injected() ? "the test shape" : "the record",
                  (unsigned long)shape->lengthMs, digits);
  } else {
    Serial.println("[wave] it shows a plain line");
  }
  // The counters.
  const uint32_t reads = state_.reads();
  Serial.printf("[wave] since the boot: %lu reads (%lu hits, %lu misses, %lu damaged, %lu absent, %lu failed, %lu "
                "dropped), probes mean %.1f, the job's ms mean %lu, max %lu; %lu remounts\n",
                (unsigned long)reads, (unsigned long)state_.hits(), (unsigned long)state_.misses(),
                (unsigned long)state_.damaged(), (unsigned long)state_.absents(), (unsigned long)state_.ioErrors(),
                (unsigned long)state_.dropped(), reads ? static_cast<double>(state_.slotReads()) / reads : 0.0,
                (unsigned long)state_.meanJobMs(), (unsigned long)state_.maxJobMs(), (unsigned long)remounts_);
  Serial.printf("[wave] gw=<%d digits 0-9> a test shape, gw#<%u hex> one from %u bars, gw- drops it (gw alone walks "
                "the card)\n",
                wave::kColumns, (unsigned)(2 * mpwf::kBarCount), (unsigned)mpwf::kBarCount);
}
