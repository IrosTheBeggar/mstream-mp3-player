// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CardRoot.h"

#include <cstdio>
#include <cstring>

#include "CardTags.h"

namespace cardroot {

namespace cc = cardcontract;

namespace {

constexpr const char* kManifestBin = "/.mstream/manifest.bin";
constexpr const char* kManifestTmp = "/.mstream/manifest.tmp";
constexpr const char* kPendingBin = "/.mstream/pending.bin";
constexpr const char* kPendingTmp = "/.mstream/pending.tmp";
constexpr const char* kDir = "/.mstream/";

// `path` for reading; nullptr, and `why`: Missing, or Io when the card failed.
tagstore::File* openOr(tagstore::Fs& fs, const char* path, cc::Why* why) {
  bool cardFailed = false;
  tagstore::File* f = fs.openRead(path, &cardFailed);
  if (!f) *why = cardFailed ? cc::Why::Io : cc::Why::Missing;
  return f;
}

// What a reader's Io comes to: on a file whose own records are damaged it is the
// file's fault, as a changed byte is, and a boot that took it for the card's would
// hold on to it for ever.
cc::Why ofFile(const tagstore::File& f, cc::Why w) {
  return w == cc::Why::Io && f.damaged() ? cc::Why::FatRecords : w;
}

// Opens `path` and checks it as a root. Ok: `m` holds it.
cc::Why openRoot(tagstore::Fs& fs, const char* path, cc::msmf::Manifest& m, uint8_t* scratch, uint32_t bytes) {
  cc::Why w = cc::Why::Ok;
  tagstore::File* f = openOr(fs, path, &w);
  if (!f) return w;
  w = ofFile(*f, m.open(*f, scratch, bytes));
  fs.close(f);
  return w;
}

cc::Why openPlan(tagstore::Fs& fs, const char* path, uint8_t* scratch, uint32_t bytes) {
  cc::Why w = cc::Why::Ok;
  tagstore::File* f = openOr(fs, path, &w);
  if (!f) return w;
  cc::mspd::Plan p;
  w = p.open(*f, scratch, bytes);
  fs.close(f);
  return w;
}

}  // namespace

void read(tagstore::Fs& fs, uint8_t* scratch, uint32_t scratchBytes, Root* out, const uint8_t* acksOwn) {
  Root& r = *out;
  r.hasRoot = false;
  r.cardId = 0;
  r.generation = 0;
  r.commitTime = 0;
  r.present = false;
  r.identity = tagstore::Identity();
  r.tagsPath[0] = 0;
  r.tagsBytes = 0;
  r.tagsRecords = 0;
  r.why = cc::Why::Missing;
  r.sameCommitTwice = false;
  r.rootCount = 0;
  r.plan = false;
  r.autoDj = Root::AutoDj();
  r.waves = Root::Waves();
  r.acks = cc::msmf::Acks();
  r.acksWhy = cc::Why::Missing;

  // The plan: pending.bin, else a valid pending.tmp (2.12.5).
  r.plan = fs.exists(kPendingBin) || openPlan(fs, kPendingTmp, scratch, scratchBytes) == cc::Why::Ok;

  // The election (2.5.4): each candidate checked whole.
  cc::msmf::Manifest m;
  cc::RootCandidate cand[2];
  cc::Why whys[2] = {cc::Why::Missing, cc::Why::Missing};
  const char* const names[2] = {kManifestBin, kManifestTmp};
  for (int k = 0; k < 2; ++k) {
    whys[k] = openRoot(fs, names[k], m, scratch, scratchBytes);
    if (whys[k] == cc::Why::Ok) cand[k] = m.candidate();
  }
  // A read that failed says nothing of its candidate, which may be the newer
  // of the two: no election over the other alone.
  if (whys[0] == cc::Why::Io || whys[1] == cc::Why::Io) {
    r.why = cc::Why::Io;
    return;
  }
  const cc::Election e = cc::elect(cand[0], cand[1]);
  if (e.pick == cc::Pick::None) {
    // No root: Missing when neither is there, else why the .bin (or the
    // .tmp alone) failed.
    r.why = whys[0] != cc::Why::Missing ? whys[0] : whys[1];
    return;
  }
  r.sameCommitTwice = e.sameCommit;
  // The winner again (`m` holds the last one opened), kept open while its
  // library roots are read (root() reads through the file open() checked).
  const int k = e.pick == cc::Pick::Bin ? 0 : 1;
  tagstore::File* f = openOr(fs, names[k], &r.why);
  if (!f) return;
  r.why = ofFile(*f, m.open(*f, scratch, scratchBytes));
  if (r.why == cc::Why::Ok) {
    r.hasRoot = true;
    r.cardId = m.frame().cardId;
    r.generation = m.frame().generation;
    r.commitTime = m.commitTime();
    const uint32_t roots = m.rootCount();
    for (uint32_t i = 0; i < roots && r.rootCount < kMaxRoots; ++i) {
      const cc::Why w = m.root(i, r.roots[r.rootCount], sizeof(r.roots[0]));
      if (w == cc::Why::Ok) ++r.rootCount;
      if (w == cc::Why::Io) {
        r.why = ofFile(*f, w);  // (a root the card kept back would index its files under /music)
        break;
      }
    }
    // ACKS, on its own: one that fails is none (the root stays valid).
    r.acksWhy = cc::msmf::readAcks(*f, scratch, scratchBytes, &r.acks, acksOwn);
  }
  fs.close(f);
  if (r.why != cc::Why::Ok) return;

  // T: the MPTG companion, its frame and header against COMP's entry.
  const cc::msmf::Comp& comp = m.tags();
  std::snprintf(r.tagsPath, sizeof(r.tagsPath), "%s%s", kDir, m.tagsName());
  tagstore::File* t = openOr(fs, r.tagsPath, &r.why);
  if (!t) return;
  cc::Container c;
  cc::mptg::Info info;
  r.why = ofFile(*t, cc::mptg::openFile(c, *t, &info));
  r.tagsBytes = t->size();
  fs.close(t);
  if (r.why != cc::Why::Ok) return;
  if (!cc::msmf::companionMatches(comp, info.frame, info.source, nullptr)) {
    r.why = cc::Why::Comp;
    return;
  }
  r.present = true;
  r.tagsRecords = info.recordCount;
  r.identity.present = true;
  r.identity.cardId = m.frame().cardId;
  r.identity.generation = m.frame().generation;
  r.identity.commitId = m.commitId();
  r.identity.tagsCrc = info.frame.headerCrc;

  // The MPDJ entry, kept while T is present (AUTODJ-INDEX.md 7.2). Its
  // name is the one open() checked against the entry's generation.
  if (m.hasAutoDj()) {
    r.autoDj.present = true;
    r.autoDj.entry = m.autoDj();
    std::snprintf(r.autoDj.path, sizeof(r.autoDj.path), "%s%s", kDir, m.autoDjName());
  }
  // The MPWF entry, likewise (WAVEFORM-INDEX.md 7.2).
  if (m.hasWaves()) {
    r.waves.present = true;
    r.waves.entry = m.waves();
    std::snprintf(r.waves.path, sizeof(r.waves.path), "%s%s", kDir, m.wavesName());
  }
}

uint32_t readAtBoot(tagstore::Fs& fs, uint8_t* scratch, uint32_t scratchBytes, Root* out, void (*wait)(uint32_t ms),
                    const uint8_t* acksOwn) {
  read(fs, scratch, scratchBytes, out, acksOwn);
  uint32_t reads = 1;
  for (uint32_t ms : kBootWaitsMs) {
    if (!out->readFailed()) break;
    wait(ms);
    read(fs, scratch, scratchBytes, out, acksOwn);
    ++reads;
  }
  return reads;
}

size_t describe(const Root& r, char* buf, size_t size) {
  if (!size) return 0;
  int n;
  if (r.readFailed() && r.hasRoot && r.tagsPath[0]) {
    // The root was read, and names the card (@status cid=): only its T wasn't.
    const char* name = std::strrchr(r.tagsPath, '/');
    n = std::snprintf(buf, size, "its T %s, generation %lu, unread (a read of the card failed)",
                      name ? name + 1 : r.tagsPath, static_cast<unsigned long>(r.generation));
  } else if (r.readFailed()) {
    n = std::snprintf(buf, size, "its root unread (a read of the card failed)");
  } else if (!r.present && r.hasRoot && r.tagsPath[0]) {
    // A root that names the card (@status cid=) and a T that failed: which
    // T, of which generation, and why ("missing": not on the card).
    const char* name = std::strrchr(r.tagsPath, '/');
    n = std::snprintf(buf, size, "no transfer data (the root's T %s, generation %lu: %s)",
                      name ? name + 1 : r.tagsPath, static_cast<unsigned long>(r.generation), cc::whyName(r.why));
  } else if (!r.present) {
    n = std::snprintf(buf, size, "no transfer data (%s)",
                      r.why == cc::Why::Missing ? "none on the card" : cc::whyName(r.why));
  } else {
    const char* name = std::strrchr(r.tagsPath, '/');
    n = std::snprintf(buf, size, "T %s, generation %lu (commit %08lx%08lx), %lu library root%s%s",
                      name ? name + 1 : r.tagsPath, static_cast<unsigned long>(r.identity.generation),
                      static_cast<unsigned long>(r.identity.commitId >> 32),
                      static_cast<unsigned long>(r.identity.commitId & 0xFFFFFFFFu),
                      static_cast<unsigned long>(r.rootCount), r.rootCount == 1 ? "" : "s",
                      r.sameCommitTwice ? "; manifest.bin and .tmp are one commit (a cut rename)" : "");
  }
  if (n < 0) return 0;
  const size_t len = static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1;
  if (r.plan && len + 1 < size) {
    const int more = std::snprintf(buf + len, size - len, "; a transfer's plan is on the card (it didn't finish)");
    if (more > 0) return len + static_cast<size_t>(more) < size ? len + static_cast<size_t>(more) : size - 1;
  }
  return len;
}

}  // namespace cardroot
