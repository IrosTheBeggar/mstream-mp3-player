// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CardManifest.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cardcontract {

namespace {

// Reads the string at `off` of a STRS stream read front to back: 0 is the
// empty string; any other offset must be inside, at or past the stream's
// position (2.4.4's order), with a NUL after it inside.
Why nextString(Stream& strs, const Section& sec, uint32_t off, char* out, size_t cap, size_t* len) {
  if (cap) out[0] = 0;
  if (len) *len = 0;
  if (off == 0) return Why::Ok;
  if (off >= sec.bytes) return Why::String;
  if (sec.offset + off < strs.position()) return Why::StringOrder;
  if (!strs.skipTo(sec.offset + off) || !strs.readString(out, cap, len)) return Why::String;
  return Why::Ok;
}

class Bytes {
public:
  uint32_t add(const std::string& s) {
    if (s.empty()) return 0;
    const uint32_t off = static_cast<uint32_t>(v.size());
    v.insert(v.end(), s.begin(), s.end());
    v.push_back(0);
    return off;
  }
  std::vector<uint8_t> v{0};
};

bool emit(Sink& out, const FileMeta& fm, const uint8_t* th, uint32_t headerBytes, const SectionOut* s,
          const std::vector<uint8_t>* const* data, uint32_t n, uint32_t* fileBytes, uint32_t* headerCrc) {
  ContainerWriter cw;
  if (!cw.begin(out, fm, th, headerBytes, s, n)) return false;
  for (uint32_t i = 0; i < n; ++i)
    if (s[i].bytes && !cw.write(data[i]->data(), s[i].bytes)) return false;
  return cw.finish(fileBytes, headerCrc);
}

}  // namespace

// ---------------------------------------------------------------------------
// MSMF
// ---------------------------------------------------------------------------
namespace msmf {

namespace {
const SectionSpec kSections[kSecCount] = {
    {kComp, kCompStride, true},
    {kLibr, kLibrStride, false},
    {kStrs, 0, true},
};
}  // namespace

const FormatSpec kSpec = {kMagicMsmf, kHeaderBytes, kSections, kSecCount};

bool companionName(uint32_t kind, uint32_t generation, char* out, size_t cap) {
  const char* stem = kind == kMagicMptg ? "tags" : (kind == kMagicMpdj ? "autodj" : nullptr);
  if (!stem) return false;
  const int n = std::snprintf(out, cap, "%s-%08x.bin", stem, static_cast<unsigned>(generation));
  return n > 0 && static_cast<size_t>(n) < cap;
}

namespace {
// ^tags-[0-9a-f]{8}\.bin$ (or autodj-) whose digits are `generation`.
bool nameIs(uint32_t kind, uint32_t generation, const char* name, size_t len) {
  char want[Manifest::kNameBytes];
  if (!companionName(kind, generation, want, sizeof(want))) return false;
  return std::strlen(want) == len && std::memcmp(want, name, len) == 0;
}
}  // namespace

Why Manifest::open(Source& src, uint8_t* scratch, uint32_t scratchBytes) {
  tags_ = Comp();
  autoDj_ = Comp();
  tagsName_[0] = autoDjName_[0] = producer_[0] = serverRevision_[0] = 0;
  if (!scratch || scratchBytes < 96) return Why::Io;
  uint8_t th[kHeaderBytes - kCommonHeaderBytes];
  Why w = c_.open(src, kSpec, th);
  if (w != Why::Ok) return w;
  frame_ = c_.header();
  commitTime_ = get32(th);
  flags_ = get32(th + 4);
  commitId_ = get64(th + 8);
  std::memcpy(serverInstance_, th + 16, 16);
  const uint32_t producer = get32(th + 32);
  const uint32_t revision = get32(th + 36);
  baseGeneration_ = get32(th + 40);
  serverUrlKey_ = get64(th + 48);

  const Section& comp = c_.section(kSecComp);
  const Section& libr = c_.section(kSecLibr);
  const Section& strs = c_.section(kSecStrs);
  const uint32_t part = scratchBytes / 3;
  Stream cs, ls, ss;
  cs.begin(&src, comp.offset, comp.offset + comp.bytes, scratch, part);
  ls.begin(&src, libr.offset, libr.offset + libr.bytes, scratch + part, part);
  ss.begin(&src, strs.offset, strs.offset + strs.bytes, scratch + 2 * part, part);
  uint8_t b;
  if (strs.bytes == 0 || !ss.readByte(&b) || b != 0) return Why::String;
  if ((w = nextString(ss, strs, producer, producer_, sizeof(producer_), nullptr)) != Why::Ok) return w;
  if ((w = nextString(ss, strs, revision, serverRevision_, sizeof(serverRevision_), nullptr)) != Why::Ok) return w;

  for (uint32_t i = 0; i < comp.count; ++i) {
    uint8_t e[kCompStride];
    if (!cs.read(e, kCompStride) || !cs.skipTo(cs.position() + comp.stride - kCompStride)) return Why::Io;
    Comp c;
    c.kind = get32(e);
    c.generation = get32(e + 4);
    c.fileBytes = get32(e + 8);
    c.headerCrc = get32(e + 12);
    c.name = get32(e + 16);
    std::memcpy(c.key, e + 24, 16);
    char name[kMaxRelPath + 1];
    size_t len = 0;
    if ((w = nextString(ss, strs, c.name, name, sizeof(name), &len)) != Why::Ok) return w;
    if (c.kind == kMagicMptg) {
      if (tags_.kind || autoDj_.kind) return Why::Comp;
      if (!nameIs(c.kind, c.generation, name, len)) return Why::Comp;
      tags_ = c;
      std::memcpy(tagsName_, name, len + 1);
    } else if (c.kind == kMagicMpdj) {
      if (!tags_.kind || autoDj_.kind) return Why::Comp;
      if (!nameIs(c.kind, c.generation, name, len)) return Why::Comp;
      autoDj_ = c;
      std::memcpy(autoDjName_, name, len + 1);
    }
    // Any other kind: ignored (2.4.5), its name read in its place.
  }
  if (!tags_.kind) return Why::Comp;

  char prev[kMaxRelPath + 1];
  size_t prevLen = 0;
  for (uint32_t i = 0; i < libr.count; ++i) {
    uint8_t e[kLibrStride];
    if (!ls.read(e, kLibrStride) || !ls.skipTo(ls.position() + libr.stride - kLibrStride)) return Why::Io;
    const uint32_t off = get32(e);
    char root[kMaxRelPath + 2];
    size_t len = 0;
    if (off == 0) return Why::Libr;
    if ((w = nextString(ss, strs, off, root, sizeof(root), &len)) != Why::Ok) return w;
    if (!validRelPath(root, len)) return Why::Libr;
    if (i > 0 && compareNames(prev, prevLen, root, len) >= 0) return Why::Libr;
    std::memcpy(prev, root, len);
    prevLen = len;
  }
  if (!cs.drain() || !ls.drain() || !ss.drain()) return Why::Io;
  if (cs.crc() != comp.crc || (libr.present() && ls.crc() != libr.crc) || ss.crc() != strs.crc) return Why::SectionCrc;
  return Why::Ok;
}

Why Manifest::root(uint32_t i, char* out, size_t cap) const {
  const Section& libr = c_.section(kSecLibr);
  if (cap) out[0] = 0;
  if (i >= libr.count) return Why::Libr;
  uint8_t e[kLibrStride];
  if (!c_.source()->read(libr.offset + i * libr.stride, e, kLibrStride)) return Why::Io;
  return readStringAt(*c_.source(), c_.section(kSecStrs), get32(e), out, cap);
}

RootCandidate Manifest::candidate() const {
  RootCandidate r;
  r.valid = true;
  r.generation = frame_.generation;
  r.commitId = commitId_;
  r.headerCrc = frame_.headerCrc;
  return r;
}

bool companionMatches(const Comp& entry, const Header& companion, uint8_t mptgSource, const uint8_t* mpdjSig) {
  if (companion.magic != entry.kind) return false;
  if (companion.generation != entry.generation || companion.fileBytes != entry.fileBytes ||
      companion.headerCrc != entry.headerCrc)
    return false;
  if (entry.kind == kMagicMptg) return mptgSource == 2 || mptgSource == 3;
  if (entry.kind == kMagicMpdj) return mpdjSig && std::memcmp(mpdjSig, entry.key, 16) == 0;
  return false;
}

bool write(Sink& out, const ManifestIn& in, uint32_t* fileBytes, uint32_t* headerCrc, const char** error) {
  auto bad = [&](const char* e) {
    if (error) *error = e;
    return false;
  };
  if (in.compCount < 1 || in.compCount > 2 || in.comps[0].kind != kMagicMptg ||
      (in.compCount == 2 && in.comps[1].kind != kMagicMpdj))
    return bad("COMP: one MPTG, then at most one MPDJ");
  std::vector<std::string> roots;
  for (uint32_t i = 0; i < in.rootCount; ++i) {
    const std::string r = in.roots[i] ? in.roots[i] : "";
    if (!validRelPath(r.data(), r.size())) return bad("LIBR root");
    roots.push_back(r);
  }
  std::sort(roots.begin(), roots.end(), [](const std::string& a, const std::string& b) {
    return compareNames(a.data(), a.size(), b.data(), b.size()) < 0;
  });
  for (size_t i = 1; i < roots.size(); ++i)
    if (roots[i] == roots[i - 1]) return bad("LIBR root twice");

  Bytes strs;
  const uint32_t producer = strs.add(in.producer ? in.producer : "");
  const uint32_t revision = strs.add(in.serverRevision ? in.serverRevision : "");
  std::vector<uint8_t> comp(in.compCount * kCompStride, 0);
  for (uint32_t i = 0; i < in.compCount; ++i) {
    char name[Manifest::kNameBytes];
    if (!companionName(in.comps[i].kind, in.comps[i].generation, name, sizeof(name))) return bad("COMP name");
    uint8_t* e = comp.data() + i * kCompStride;
    put32(e, in.comps[i].kind);
    put32(e + 4, in.comps[i].generation);
    put32(e + 8, in.comps[i].fileBytes);
    put32(e + 12, in.comps[i].headerCrc);
    put32(e + 16, strs.add(name));
    if (in.comps[i].kind == kMagicMpdj) std::memcpy(e + 24, in.comps[i].key, 16);
  }
  std::vector<uint8_t> libr(roots.size() * kLibrStride);
  for (size_t i = 0; i < roots.size(); ++i) put32(libr.data() + i * kLibrStride, strs.add(roots[i]));

  uint8_t th[kHeaderBytes - kCommonHeaderBytes] = {};
  put32(th, in.commitTime);
  put32(th + 4, in.flags);
  put64(th + 8, in.commitId);
  std::memcpy(th + 16, in.serverInstance, 16);
  put32(th + 32, producer);
  put32(th + 36, revision);
  put32(th + 40, in.baseGeneration);
  put64(th + 48, in.serverUrlKey);

  SectionOut s[3];
  const std::vector<uint8_t>* data[3];
  uint32_t n = 0;
  s[n] = SectionOut{kComp, kSectionRequired, in.compCount, kCompStride, static_cast<uint32_t>(comp.size())};
  data[n++] = &comp;
  if (!roots.empty()) {
    s[n] = SectionOut{kLibr, 0, static_cast<uint32_t>(roots.size()), kLibrStride, static_cast<uint32_t>(libr.size())};
    data[n++] = &libr;
  }
  s[n] = SectionOut{kStrs, kSectionRequired, 0, 0, static_cast<uint32_t>(strs.v.size())};
  data[n++] = &strs.v;
  FileMeta fm;
  fm.magic = kMagicMsmf;
  fm.minor = in.minor;
  fm.generation = in.generation;
  fm.cardId = in.cardId;
  if (!emit(out, fm, th, kHeaderBytes, s, data, n, fileBytes, headerCrc)) return bad("sink");
  return true;
}

}  // namespace msmf

// ---------------------------------------------------------------------------
// MSPD
// ---------------------------------------------------------------------------
namespace mspd {

namespace {
const SectionSpec kSections[kSecCount] = {
    {kPend, kPendStride, true},
    {kStrs, 0, true},
};

// The deletes, then the folders, then the writes.
int rank(uint8_t op) { return op == kOpDelete ? 0 : (op == kOpFolder ? 1 : 2); }

int comparePaths(uint8_t op, const char* a, size_t an, const char* b, size_t bn) {
  return op == kOpFolder ? compareFolderPaths(a, an, b, bn) : compareFilePaths(a, an, b, bn);
}
}  // namespace

const FormatSpec kSpec = {kMagicMspd, kHeaderBytes, kSections, kSecCount};

Why Plan::open(Source& src, uint8_t* scratch, uint32_t scratchBytes) {
  ops_ = 0;
  unknownOp_ = false;
  if (!scratch || scratchBytes < 64) return Why::Io;
  uint8_t th[kHeaderBytes - kCommonHeaderBytes];
  Why w = c_.open(src, kSpec, th);
  if (w != Why::Ok) return w;
  frame_ = c_.header();
  runId_ = get64(th);
  baseGeneration_ = get32(th + 8);
  const Section& pend = c_.section(kSecPend);
  const Section& strs = c_.section(kSecStrs);
  const uint32_t part = scratchBytes / 2;
  Stream ps, ss;
  ps.begin(&src, pend.offset, pend.offset + pend.bytes, scratch, part);
  ss.begin(&src, strs.offset, strs.offset + strs.bytes, scratch + part, part);
  uint8_t b;
  if (strs.bytes == 0 || !ss.readByte(&b) || b != 0) return Why::String;
  char prev[kMaxRelPath + 2], path[kMaxRelPath + 2];
  size_t prevLen = 0;
  int prevRank = -1;
  for (uint32_t i = 0; i < pend.count; ++i) {
    uint8_t e[kPendStride];
    if (!ps.read(e, kPendStride) || !ps.skipTo(ps.position() + pend.stride - kPendStride)) return Why::Io;
    const uint8_t op = e[0];
    const uint32_t off = get32(e + 4);
    if (op == 0) return Why::PendOp;
    if (off == 0) return Why::PendPath;
    size_t len = 0;
    if ((w = nextString(ss, strs, off, path, sizeof(path), &len)) != Why::Ok) return w;
    if (len > kMaxRelPath) return Why::PathLength;
    if (!validRelPath(path, len)) return Why::PendPath;
    if (op > kOpFolder) {
      unknownOp_ = true;  // a newer writer's op: not placed in the order
      continue;
    }
    const int r = rank(op);
    if (r < prevRank) return Why::PendOrder;
    if (r == prevRank && comparePaths(op, prev, prevLen, path, len) >= 0) return Why::PendOrder;
    prevRank = r;
    std::memcpy(prev, path, len);
    prevLen = len;
  }
  if (!ps.drain() || !ss.drain()) return Why::Io;
  if (ps.crc() != pend.crc || ss.crc() != strs.crc) return Why::SectionCrc;
  ops_ = pend.count;
  return Why::Ok;
}

RootCandidate Plan::candidate() const {
  RootCandidate r;
  r.valid = true;
  r.generation = frame_.generation;
  r.commitId = runId_;
  r.headerCrc = frame_.headerCrc;
  return r;
}

bool Plan::op(uint32_t i, Op* out) const {
  const Section& pend = c_.section(kSecPend);
  if (i >= pend.count) return false;
  uint8_t e[kPendStride];
  if (!c_.source()->read(pend.offset + i * pend.stride, e, kPendStride)) return false;
  out->op = e[0];
  out->flags = e[1];
  out->path = get32(e + 4);
  out->expectedSize = get32(e + 8);
  return true;
}

Why Plan::path(const Op& op, char* out, size_t cap) const {
  return readStringAt(*c_.source(), c_.section(kSecStrs), op.path, out, cap);
}

bool write(Sink& out, const PlanIn& in, uint32_t* fileBytes, uint32_t* headerCrc, const char** error) {
  auto bad = [&](const char* e) {
    if (error) *error = e;
    return false;
  };
  struct E {
    uint8_t op;
    std::string path;
    uint32_t size;
  };
  std::vector<E> ops;
  for (uint32_t i = 0; i < in.opCount; ++i) {
    const OpIn& o = in.ops[i];
    if (o.op < kOpWrite || o.op > kOpFolder) return bad("op");
    const std::string p = o.path ? o.path : "";
    if (!validRelPath(p.data(), p.size())) return bad("path");
    ops.push_back(E{o.op, p, o.op == kOpWrite ? o.expectedSize : 0});
  }
  std::sort(ops.begin(), ops.end(), [](const E& a, const E& b) {
    if (rank(a.op) != rank(b.op)) return rank(a.op) < rank(b.op);
    return comparePaths(a.op, a.path.data(), a.path.size(), b.path.data(), b.path.size()) < 0;
  });
  for (size_t i = 1; i < ops.size(); ++i)
    if (ops[i].op == ops[i - 1].op && ops[i].path == ops[i - 1].path) return bad("an op twice");
  Bytes strs;
  std::vector<uint8_t> pend(ops.size() * kPendStride, 0);
  for (size_t i = 0; i < ops.size(); ++i) {
    uint8_t* e = pend.data() + i * kPendStride;
    e[0] = ops[i].op;
    put32(e + 4, strs.add(ops[i].path));
    put32(e + 8, ops[i].size);
  }
  uint8_t th[kHeaderBytes - kCommonHeaderBytes] = {};
  put64(th, in.runId);
  put32(th + 8, in.baseGeneration);
  SectionOut s[2] = {
      SectionOut{kPend, kSectionRequired, static_cast<uint32_t>(ops.size()), kPendStride,
                 static_cast<uint32_t>(pend.size())},
      SectionOut{kStrs, kSectionRequired, 0, 0, static_cast<uint32_t>(strs.v.size())},
  };
  const std::vector<uint8_t>* data[2] = {&pend, &strs.v};
  FileMeta fm;
  fm.magic = kMagicMspd;
  fm.minor = in.minor;
  fm.generation = in.generation;
  fm.cardId = in.cardId;
  if (!emit(out, fm, th, kHeaderBytes, s, data, 2, fileBytes, headerCrc)) return bad("sink");
  return true;
}

}  // namespace mspd

}  // namespace cardcontract
