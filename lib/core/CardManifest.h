// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardContainer.h"
#include "CardContract.h"

// The transfer's root and its plan (docs/METADATA.md 2.5, 2.12.5).
//
// manifest.bin (MSMF) is the commit point: a small file naming the commit's
// companion files (COMP: the MPTG tags file, at most one MPDJ table) by
// their generation, length and header CRC, and the library roots (LIBR) of
// the vpath layout. The device reads it with two small reads at every boot
// (manifest.bin and manifest.tmp, the election in CardContainer.h), then
// the named companion's header. Manifest::open() checks a root whole (its
// sections are a few hundred bytes); companionMatches() says whether a
// companion file is the one the root names.
//
// pending.bin (MSPD) is an unfinished run's plan: the paths it will delete,
// the folders it will create, the paths it will write. The device reads
// only its presence ("the last transfer didn't finish"); the software
// recovers from it (2.12.3).
namespace cardcontract {

namespace msmf {

constexpr uint32_t kHeaderBytes = 96;
constexpr uint32_t kComp = fourcc("COMP");
constexpr uint32_t kLibr = fourcc("LIBR");
constexpr uint32_t kCompStride = 40;
constexpr uint32_t kLibrStride = 4;
enum Sec : uint32_t { kSecComp = 0, kSecLibr, kSecStrs, kSecCount };
extern const FormatSpec kSpec;

constexpr uint32_t kFinal = 1;  // flags: this commit ended a run (clear: a checkpoint)

struct Comp {
  uint32_t kind = 0;  // kMagicMptg or kMagicMpdj (others are ignored)
  uint32_t generation = 0;
  uint32_t fileBytes = 0;
  uint32_t headerCrc = 0;
  uint32_t name = 0;  // STRS offset
  uint8_t key[16] = {};  // MPDJ: its selection signature; zeros for MPTG
};

// A root, checked whole; its fields and companions kept, its library roots
// read on demand (root()).
class Manifest {
public:
  static constexpr size_t kNameBytes = 24;  // "autodj-0000002a.bin" and its NUL

  // Checks `src` as a root (2.4.3): the frame; COMP's kinds (unknown kinds
  // ignored; exactly one MPTG, then at most one MPDJ), their names
  // (^tags-[0-9a-f]{8}\.bin$, ^autodj-[0-9a-f]{8}\.bin$, the digits the
  // entry's generation); LIBR's roots (relative paths, strictly increasing
  // by bytes); STRS in 2.4.4's order; every section's CRC. `scratch`: at
  // least 96 bytes, split between the three sections' streams.
  Why open(Source& src, uint8_t* scratch, uint32_t scratchBytes);
  const Header& frame() const { return frame_; }
  uint32_t commitTime() const { return commitTime_; }
  uint32_t flags() const { return flags_; }
  uint64_t commitId() const { return commitId_; }
  const uint8_t* serverInstance() const { return serverInstance_; }
  const char* producer() const { return producer_; }
  const char* serverRevision() const { return serverRevision_; }
  uint32_t baseGeneration() const { return baseGeneration_; }
  uint64_t serverUrlKey() const { return serverUrlKey_; }
  // The MPTG companion (always there in a valid root) and the MPDJ one.
  const Comp& tags() const { return tags_; }
  const char* tagsName() const { return tagsName_; }
  bool hasAutoDj() const { return autoDj_.kind != 0; }
  const Comp& autoDj() const { return autoDj_; }
  const char* autoDjName() const { return autoDjName_; }
  // The library roots, relative to /music, in LIBR's order (none: /music is
  // the only root). root() reads one from the file open() checked.
  uint32_t rootCount() const { return c_.section(kSecLibr).count; }
  Why root(uint32_t i, char* out, size_t cap) const;
  const Container& container() const { return c_; }
  // This root as a candidate in the election.
  RootCandidate candidate() const;

private:
  Container c_;
  Header frame_;
  uint32_t commitTime_ = 0, flags_ = 0;
  uint64_t commitId_ = 0;
  uint8_t serverInstance_[16] = {};
  char producer_[64] = "";
  char serverRevision_[64] = "";
  uint32_t baseGeneration_ = 0;
  uint64_t serverUrlKey_ = 0;
  Comp tags_, autoDj_;
  char tagsName_[kNameBytes] = "";
  char autoDjName_[kNameBytes] = "";
};

// The companion file's name for its kind and generation: "tags-0000002a.bin",
// "autodj-0000002a.bin"; false for another kind or a short buffer.
bool companionName(uint32_t kind, uint32_t generation, char* out, size_t cap);
// A companion is the root's (2.5.3) when it validates (the caller's check)
// and its header's generation, fileBytes and headerCrc are the entry's, and
// for an MPDJ its selection signature is the entry's key; an MPTG companion
// must be the software's (source 2 or 3).
bool companionMatches(const Comp& entry, const Header& companion, uint8_t mptgSource, const uint8_t* mpdjSig);

// Writing a root.
struct CompIn {
  uint32_t kind = 0;
  uint32_t generation = 0;
  uint32_t fileBytes = 0;
  uint32_t headerCrc = 0;
  uint8_t key[16] = {};
};
struct ManifestIn {
  uint32_t generation = 0;
  uint64_t cardId = 0;
  uint16_t minor = 0;
  uint32_t commitTime = 0;
  uint32_t flags = 0;
  uint64_t commitId = 0;
  uint8_t serverInstance[16] = {};
  const char* producer = "";
  const char* serverRevision = "";
  uint32_t baseGeneration = 0;
  uint64_t serverUrlKey = 0;
  const CompIn* comps = nullptr;  // the MPTG, then the MPDJ if any
  uint32_t compCount = 0;
  const char* const* roots = nullptr;  // LIBR roots, any order (sorted by the writer)
  uint32_t rootCount = 0;
};
bool write(Sink& out, const ManifestIn& in, uint32_t* fileBytes, uint32_t* headerCrc, const char** error);

}  // namespace msmf

namespace mspd {

constexpr uint32_t kHeaderBytes = 56;
constexpr uint32_t kPend = fourcc("PEND");
constexpr uint32_t kPendStride = 16;
enum Sec : uint32_t { kSecPend = 0, kSecStrs, kSecCount };
extern const FormatSpec kSpec;

constexpr uint8_t kOpWrite = 1;
constexpr uint8_t kOpDelete = 2;
constexpr uint8_t kOpFolder = 3;

struct Op {
  uint8_t op = 0;
  uint8_t flags = 0;
  uint32_t path = 0;  // STRS offset, relative to /music
  uint32_t expectedSize = 0;
};

// A plan, checked whole (2.4.3): the frame; PEND in 2.12.5's order (the
// deletes, then the folders, then the writes, each group strictly in
// canonical order, the folders in pre-order; an op above 3 is a newer
// writer's and isn't placed, an op of 0 makes the plan absent); its paths
// relative, made of names, within 255 bytes with "/music/"; STRS in PEND's
// order; the CRCs. stopsSoftware(): an op above 3, which stops the
// software as an unknown major does (2.4.5). `scratch`: at least 64 bytes.
class Plan {
public:
  Why open(Source& src, uint8_t* scratch, uint32_t scratchBytes);
  const Header& frame() const { return frame_; }
  uint64_t runId() const { return runId_; }
  uint32_t baseGeneration() const { return baseGeneration_; }
  uint32_t opCount() const { return ops_; }
  bool stopsSoftware() const { return unknownOp_; }
  RootCandidate candidate() const;
  const Container& container() const { return c_; }
  // Entry i, and its path (relative to /music).
  bool op(uint32_t i, Op* out) const;
  Why path(const Op& op, char* out, size_t cap) const;

private:
  Container c_;
  Header frame_;
  uint64_t runId_ = 0;
  uint32_t baseGeneration_ = 0;
  uint32_t ops_ = 0;
  bool unknownOp_ = false;
};

struct OpIn {
  uint8_t op = 0;
  const char* path = "";
  uint32_t expectedSize = 0;  // 0 for deletes and folders
};
struct PlanIn {
  uint32_t generation = 0;
  uint64_t cardId = 0;
  uint16_t minor = 0;
  uint64_t runId = 0;
  uint32_t baseGeneration = 0;
  const OpIn* ops = nullptr;  // any order (sorted by the writer)
  uint32_t opCount = 0;
};
bool write(Sink& out, const PlanIn& in, uint32_t* fileBytes, uint32_t* headerCrc, const char** error);

}  // namespace mspd

}  // namespace cardcontract
