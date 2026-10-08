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

// Opens `path` and checks it as a root. Ok: `m` holds it.
cc::Why openRoot(tagstore::Fs& fs, const char* path, cc::msmf::Manifest& m, uint8_t* scratch, uint32_t bytes) {
  tagstore::File* f = fs.open(path, tagstore::Fs::Mode::Read);
  if (!f) return cc::Why::Missing;
  const cc::Why w = m.open(*f, scratch, bytes);
  fs.close(f);
  return w;
}

cc::Why openPlan(tagstore::Fs& fs, const char* path, uint8_t* scratch, uint32_t bytes) {
  tagstore::File* f = fs.open(path, tagstore::Fs::Mode::Read);
  if (!f) return cc::Why::Missing;
  cc::mspd::Plan p;
  const cc::Why w = p.open(*f, scratch, bytes);
  fs.close(f);
  return w;
}

}  // namespace

void read(tagstore::Fs& fs, uint8_t* scratch, uint32_t scratchBytes, Root* out) {
  Root& r = *out;
  r.present = false;
  r.identity = tagstore::Identity();
  r.tagsPath[0] = 0;
  r.tagsBytes = 0;
  r.tagsRecords = 0;
  r.why = cc::Why::Missing;
  r.sameCommitTwice = false;
  r.rootCount = 0;
  r.plan = false;

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
  tagstore::File* f = fs.open(names[k], tagstore::Fs::Mode::Read);
  if (!f) return;
  r.why = m.open(*f, scratch, scratchBytes);
  if (r.why == cc::Why::Ok) {
    const uint32_t roots = m.rootCount();
    for (uint32_t i = 0; i < roots && r.rootCount < kMaxRoots; ++i)
      if (m.root(i, r.roots[r.rootCount], sizeof(r.roots[0])) == cc::Why::Ok) ++r.rootCount;
  }
  fs.close(f);
  if (r.why != cc::Why::Ok) return;

  // T: the MPTG companion, its frame and header against COMP's entry.
  const cc::msmf::Comp& comp = m.tags();
  std::snprintf(r.tagsPath, sizeof(r.tagsPath), "%s%s", kDir, m.tagsName());
  tagstore::File* t = fs.open(r.tagsPath, tagstore::Fs::Mode::Read);
  if (!t) {
    r.why = cc::Why::Missing;
    return;
  }
  cc::Container c;
  cc::mptg::Info info;
  r.why = cc::mptg::openFile(c, *t, &info);
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
}

size_t describe(const Root& r, char* buf, size_t size) {
  if (!size) return 0;
  int n;
  if (!r.present) {
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
