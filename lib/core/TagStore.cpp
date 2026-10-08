// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TagStore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace tagstore {

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;

namespace {

void* defaultAlloc(size_t n) { return std::malloc(n); }
void defaultFree(void* p) { std::free(p); }

// tags.jnl
constexpr uint32_t kJnlMagic = cc::fourcc("MPJC");
constexpr uint32_t kChunkHeader = 32;
constexpr uint32_t kChunkTrailer = 4;  // the CRC
// walk.jnl
constexpr uint32_t kWalkMagic = cc::fourcc("MPWJ");
constexpr uint32_t kBlockMagic = cc::fourcc("MPWB");
constexpr uint32_t kWalkHeader = 64;
constexpr uint16_t kWalkVersion = 1;
constexpr uint32_t kBlockHeader = 16;
constexpr uint8_t kBlockEntries = 1;
constexpr uint8_t kBlockEnd = 2;
constexpr uint32_t kEndPayload = 8;  // i32 skew, u32 flags
constexpr uint32_t kEndUnsettled = 1;  // an End's flag (run 2's): a doubt couldn't be settled
constexpr uint32_t kEntryHead = 4;   // kind, flags, u16 path length
constexpr uint32_t kFileTail = 16;   // a walk File entry: size, fatTime, qfp
constexpr uint32_t kDoubtTail = 32;  // a Doubt: size, fatTime, delta, T's qfp, D's qfp
constexpr uint32_t kDhdrVersion = 1;
constexpr uint32_t kDhdrWalked = 1;
constexpr uint32_t kDhdrTransfer = 2;
constexpr uint32_t kWalkTransfer = 2;  // walk.jnl's flags: the identity is a commit

// D: the contract's sections and the device's, all of which D must have
// (HIDX when it has records).
enum DevSec : uint32_t { kDFold = 0, kDRecs, kDStrs, kDHidx, kDDsta, kDDfld, kDDhdr, kDCount };
const cc::SectionSpec kDevSections[kDCount] = {
    {mptg::kFold, mptg::kFoldStride, true}, {mptg::kRecs, mptg::kRecsStride, true}, {cc::kStrs, 0, true},
    {mptg::kHidx, mptg::kHidxStride, false}, {kDsta, 0, true}, {kDfld, 0, true}, {kDhdr, 0, true},
};
const cc::FormatSpec kDevSpec = {cc::kMagicMptg, mptg::kHeaderBytes, kDevSections, kDCount};

uint32_t align8(uint32_t v) { return (v + 7u) & ~7u; }

// The folder part of a path: its length ("A/B/x.mp3" gives 3; "x.mp3" 0).
size_t folderLength(const char* rel, size_t len) {
  size_t k = len;
  while (k > 0 && rel[k - 1] != '/') --k;
  return k ? k - 1 : 0;
}

// `f` is `d` or one of its ancestors ("" is everyone's).
bool ancestorOrSelf(const char* f, size_t fn, const char* d, size_t dn) {
  if (fn == 0) return true;
  if (fn > dn || std::memcmp(f, d, fn) != 0) return false;
  return fn == dn || d[fn] == '/';
}

// The order of a walk run's keys: a folder's entry before its files (and
// its subfolders'), files in canonical order, folders in pre-order.
int compareKeys(bool aFolder, const char* a, size_t an, bool bFolder, const char* b, size_t bn) {
  if (!aFolder && !bFolder) return cc::compareFilePaths(a, an, b, bn);
  if (aFolder && bFolder) return cc::compareFolderPaths(a, an, b, bn);
  const char* f = aFolder ? a : b;
  const size_t fn = aFolder ? an : bn;
  const char* p = aFolder ? b : a;
  const size_t pn = aFolder ? bn : an;
  const size_t dn = folderLength(p, pn);
  int r;
  if (ancestorOrSelf(f, fn, p, dn))
    r = -1;  // the folder comes first
  else
    r = cc::compareFolderPaths(f, fn, p, dn) < 0 ? -1 : 1;
  return aFolder ? r : -r;
}

bool validFolderPath(const char* s, size_t n) { return n == 0 || cc::validRelPath(s, n); }

// A row's record when it holds no tags: the file's size, time and qfp.
mptg::Record bare(uint32_t size, uint32_t fatTime, uint64_t qfp) {
  mptg::Record r;
  r.size = size;
  r.fatTime = fatTime;
  r.qfp = qfp;
  return r;
}

// A walk File entry's row: a status and the confirmation, nothing else.
bool walkStatus(uint8_t flags) { return (flags & ~7u) == 0; }
// A chunk record's status: Scanned or Unreadable, not confirmed.
bool scanStatus(uint8_t flags) {
  return flags == static_cast<uint8_t>(Status::Scanned) || flags == static_cast<uint8_t>(Status::Unreadable);
}

// Reads a facts entry (31 bytes, then the cover image's name).
bool readFacts(cc::Stream& s, FolderFacts* f) {
  uint8_t b[kFactsFixed];
  if (!s.read(b, kFactsFixed)) return false;
  f->digest = cc::get64(b);
  f->imageSize = cc::get32(b + 8);
  f->imageTime = cc::get32(b + 12);
  f->audio = cc::get32(b + 16);
  f->images = cc::get32(b + 20);
  f->others = cc::get32(b + 24);
  f->imageRank = b[28];
  if (b[29] & ~1u) return false;
  f->imageOwned = (b[29] & 1) != 0;
  f->imageLength = b[30];
  if (!s.read(f->image, f->imageLength)) return false;
  f->image[f->imageLength] = 0;
  return f->imageLength == 0 || cc::validName(f->image, f->imageLength);
}

// Where a string run's bytes are (2.6.5: n, then n NUL-terminated fields).
struct RunScan {
  uint32_t field = 0;  // the field the next byte belongs to
  uint32_t n = 0;
  bool haveN = false;
};

// The distinct album and artist values (MPTG's albumValues, artistValues:
// for pre-sizing only, 2.6.1): exact up to 128, then a linear-counting
// estimate over 16,384 bits. Fixed memory whatever D holds.
struct Distinct {
  static constexpr uint32_t kExact = 128;
  static constexpr uint32_t kBits = 16384;
  uint64_t exact[kExact];
  uint32_t n = 0;
  bool estimating = false;
  uint8_t bits[kBits / 8];
  void clear() {
    n = 0;
    estimating = false;
    std::memset(bits, 0, sizeof(bits));
  }
  void setBit(uint64_t h) {
    const uint32_t b = static_cast<uint32_t>(h % kBits);
    bits[b >> 3] = static_cast<uint8_t>(bits[b >> 3] | (1u << (b & 7)));
  }
  void add(uint64_t h) {
    if (!estimating) {
      for (uint32_t i = 0; i < n; ++i)
        if (exact[i] == h) return;
      if (n < kExact) {
        exact[n++] = h;
        return;
      }
      estimating = true;
      for (uint32_t i = 0; i < n; ++i) setBit(exact[i]);
    }
    setBit(h);
  }
  uint32_t count() const {
    if (!estimating) return n;
    uint32_t zeros = 0;
    for (uint32_t i = 0; i < kBits / 8; ++i)
      for (int k = 0; k < 8; ++k)
        if (!(bits[i] & (1u << k))) ++zeros;
    if (zeros == 0) return kBits * 10;
    const double m = kBits;
    const double est = -m * std::log(zeros / m);
    return est < kExact ? kExact : static_cast<uint32_t>(est + 0.5);
  }
};

// The distinct counts' hashing of a run's fields as its bytes pass: the
// artist and album-artist items, the album's whole value.
struct ValueHasher {
  Distinct* albums = nullptr;
  Distinct* artists = nullptr;
  RunScan scan;
  uint64_t h = cc::kFnvBasis;
  uint32_t len = 0;
  void begin() {
    scan = RunScan();
    h = cc::kFnvBasis;
    len = 0;
  }
  void endValue() {
    if (len) {
      if (scan.field == cc::kAlbum) {
        if (albums) albums->add(h);
      } else if (scan.field == cc::kArtist || scan.field == cc::kAlbumArtist) {
        if (artists) artists->add(h);
      }
    }
    h = cc::kFnvBasis;
    len = 0;
  }
  // False: the bytes aren't a run.
  bool feed(const uint8_t* p, uint32_t k) {
    for (uint32_t i = 0; i < k; ++i) {
      const uint8_t b = p[i];
      if (!scan.haveN) {
        scan.haveN = true;
        scan.n = b;
        continue;
      }
      if (scan.field >= scan.n) return false;
      if (b == 0) {
        endValue();
        ++scan.field;
      } else if (b == static_cast<uint8_t>(cc::kSeparator) && cc::isListField(scan.field)) {
        endValue();
      } else {
        h = (h ^ b) * cc::kFnvPrime;
        ++len;
      }
    }
    return true;
  }
  bool complete() const { return scan.haveN && scan.field == scan.n; }
};

// A section of the file being written, through a buffer: CRC'd as it goes;
// with no file it only counts (the first pass).
struct Cursor {
  File* f = nullptr;
  uint32_t base = 0;  // where buf[0] goes
  uint8_t* buf = nullptr;
  uint32_t cap = 0;
  uint32_t fill = 0;
  uint32_t crc = 0;
  uint32_t total = 0;
  bool bad = false;
  void begin(File* file, uint32_t offset, uint8_t* b, uint32_t c) {
    f = file;
    base = offset;
    buf = b;
    cap = c;
    fill = crc = total = 0;
    bad = false;
  }
  bool flush() {
    if (!f || fill == 0 || bad) return !bad;
    if (!f->write(base, buf, fill)) bad = true;
    base += fill;
    fill = 0;
    return !bad;
  }
  bool put(const void* data, uint32_t n) {
    if (bad) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = cc::crc32(p, n, crc);
    total += n;
    if (!f) return true;
    while (n) {
      if (fill == cap && !flush()) return false;
      uint32_t k = cap - fill;
      if (k > n) k = n;
      std::memcpy(buf + fill, p, k);
      fill += k;
      p += k;
      n -= k;
    }
    return true;
  }
  bool putByte(uint8_t b) { return put(&b, 1); }
};

}  // namespace

// ---------------------------------------------------------------------------
// Rows, the header, the facts.
// ---------------------------------------------------------------------------
uint8_t encodeRow(const Row& r) {
  return static_cast<uint8_t>((static_cast<uint8_t>(r.status) & 3u) | (r.confirmed ? 4u : 0u));
}

Row decodeRow(uint8_t b) {
  Row r;
  r.status = static_cast<Status>(b & 3u);
  r.confirmed = (b & 4u) != 0;
  return r;
}

LibraryBuilder::Row builderRow(const Row& r) {
  LibraryBuilder::Row b;
  b.status = r.status;
  b.confirmed = r.confirmed;
  return b;
}

void encodeHeader(const DeviceHeader& h, uint8_t out[kDhdrBytes]) {
  std::memset(out, 0, kDhdrBytes);
  cc::put32(out, kDhdrVersion);
  cc::put32(out + 4, (h.walked ? kDhdrWalked : 0u) | (h.walk.present ? kDhdrTransfer : 0u));
  cc::put64(out + 8, h.walk.cardId);
  cc::put32(out + 16, h.walk.generation);
  cc::put32(out + 20, h.walk.tagsCrc);
  cc::put64(out + 24, h.walk.commitId);
  cc::put32(out + 32, static_cast<uint32_t>(h.skew));
  cc::put32(out + 36, h.epoch);
}

bool decodeHeader(const uint8_t* in, uint32_t n, DeviceHeader* h) {
  *h = DeviceHeader();
  if (n < kDhdrBytes || cc::get32(in) != kDhdrVersion) return false;
  const uint32_t flags = cc::get32(in + 4);
  if (flags & ~(kDhdrWalked | kDhdrTransfer)) return false;
  h->walked = (flags & kDhdrWalked) != 0;
  h->walk.present = (flags & kDhdrTransfer) != 0;
  h->walk.cardId = cc::get64(in + 8);
  h->walk.generation = cc::get32(in + 16);
  h->walk.tagsCrc = cc::get32(in + 20);
  h->walk.commitId = cc::get64(in + 24);
  h->skew = static_cast<int32_t>(cc::get32(in + 32));
  h->epoch = cc::get32(in + 36);
  return true;
}

bool FolderFacts::operator==(const FolderFacts& o) const {
  return digest == o.digest && imageSize == o.imageSize && imageTime == o.imageTime && audio == o.audio &&
         images == o.images && others == o.others && imageRank == o.imageRank && imageOwned == o.imageOwned &&
         imageLength == o.imageLength && std::memcmp(image, o.image, imageLength) == 0;
}

LibraryIndex::FolderFacts FolderFacts::index() const {
  LibraryIndex::FolderFacts f;
  f.image = imageLength ? image : nullptr;
  f.imageCount = static_cast<uint8_t>(images < 0xFF ? images : 0xFF);
  const uint32_t other = images + others;
  f.otherCount = static_cast<uint16_t>(other < 0xFFFF ? other : 0xFFFF);
  f.imageOwned = imageLength && imageOwned;
  return f;
}

uint32_t encodeFacts(const FolderFacts& f, uint8_t* out) {
  cc::put64(out, f.digest);
  cc::put32(out + 8, f.imageSize);
  cc::put32(out + 12, f.imageTime);
  cc::put32(out + 16, f.audio);
  cc::put32(out + 20, f.images);
  cc::put32(out + 24, f.others);
  out[28] = f.imageRank;
  out[29] = f.imageOwned ? 1 : 0;
  out[30] = f.imageLength;
  std::memcpy(out + kFactsFixed, f.image, f.imageLength);
  return kFactsFixed + f.imageLength;
}

// ---------------------------------------------------------------------------
// The cut-rename rule (2.12.6).
// ---------------------------------------------------------------------------
bool twinName(const Names& n, int i, char* out, size_t cap) {
  if (i < 1 || i > kMaxTwins || !n.stem) return false;
  const int k = std::snprintf(out, cap, "%s.xl%d", n.stem, i);
  return k > 0 && static_cast<size_t>(k) < cap;
}

namespace {

constexpr size_t kNameCap = 96;

// The first twin name not on the card; 0 when all are taken.
int freeTwin(Fs& fs, const Names& n, char* out) {
  for (int i = 1; i <= kMaxTwins; ++i)
    if (twinName(n, i, out, kNameCap) && !fs.exists(out)) return i;
  return 0;
}

// A twin shares cluster `c` (non-zero).
bool twinShares(Fs& fs, const Names& n, uint32_t c, int except) {
  char t[kNameCap];
  for (int i = 1; i <= kMaxTwins; ++i) {
    if (i == except || !twinName(n, i, t, sizeof(t))) continue;
    if (fs.exists(t) && fs.firstCluster(t) == c) return true;
  }
  return false;
}

// Renames `from` to a free twin (a rename frees nothing).
bool toTwin(Fs& fs, const Names& n, const char* from) {
  char t[kNameCap];
  return freeTwin(fs, n, t) && fs.rename(from, t);
}

}  // namespace

Settled settle(Fs& fs, const Names& n, TmpCheck* check) {
  Settled s;
  if (fs.exists(n.tmp)) {
    const uint32_t ct = fs.firstCluster(n.tmp);
    const bool shared = ct != 0 && ((fs.exists(n.path) && fs.firstCluster(n.path) == ct) || twinShares(fs, n, ct, 0));
    if (shared) {
      s.what = toTwin(fs, n, n.tmp) ? Settle::Quarantined : Settle::Failed;
    } else if (!fs.exists(n.path) && check && check->whole(fs, n.tmp)) {
      s.what = fs.rename(n.tmp, n.path) ? Settle::Promoted : Settle::Failed;
    } else {
      s.what = fs.remove(n.tmp) ? Settle::Removed : Settle::Failed;
    }
  }
  s.twins = collectTwins(fs, n);
  return s;
}

bool prepareTmp(Fs& fs, const Names& n) {
  if (!fs.exists(n.tmp)) return true;
  const uint32_t c = fs.firstCluster(n.tmp);
  const bool shared = c != 0 && ((fs.exists(n.path) && fs.firstCluster(n.path) == c) || twinShares(fs, n, c, 0));
  return shared ? toTwin(fs, n, n.tmp) : fs.remove(n.tmp);
}

bool replace(Fs& fs, const Names& n) {
  if (fs.exists(n.path)) {
    const uint32_t c = fs.firstCluster(n.path);
    if (c != 0 && fs.exists(n.tmp) && fs.firstCluster(n.tmp) == c) return false;  // never: the tmp is new
    const bool shared = c != 0 && twinShares(fs, n, c, 0);
    if (shared ? !toTwin(fs, n, n.path) : !fs.remove(n.path)) return false;
  }
  return fs.rename(n.tmp, n.path);
}

uint32_t collectTwins(Fs& fs, const Names& n) {
  uint32_t left = 0;
  char t[kNameCap];
  for (int i = 1; i <= kMaxTwins; ++i) {
    if (!twinName(n, i, t, sizeof(t)) || !fs.exists(t)) continue;
    const uint32_t c = fs.firstCluster(t);
    bool shared = false;
    if (c != 0) {
      shared = (fs.exists(n.path) && fs.firstCluster(n.path) == c) ||
               (fs.exists(n.tmp) && fs.firstCluster(n.tmp) == c) || twinShares(fs, n, c, i);
    }
    if (shared || !fs.remove(t)) ++left;
  }
  return left;
}

// ---------------------------------------------------------------------------
// D's frame.
// ---------------------------------------------------------------------------
namespace {

Why openDev(cc::Container& c, cc::Source& src, DeviceInfo* out) {
  *out = DeviceInfo();
  uint8_t th[mptg::kHeaderBytes - cc::kCommonHeaderBytes];
  Why w = c.open(src, kDevSpec, th);
  if (w != Why::Ok) return w;
  mptg::Info& i = out->tags;
  i.frame = c.header();
  i.source = th[0];
  i.parserVersion = cc::get16(th + 2);
  i.readRules = cc::get16(th + 4);
  i.recordCount = cc::get32(th + 8);
  i.folderCount = cc::get32(th + 12);
  i.albumValues = cc::get32(th + 16);
  i.artistValues = cc::get32(th + 20);
  i.producer = cc::get32(th + 24);
  if (i.source != mptg::kSourceDevice) return Why::Enum;
  if (c.section(kDFold).count != i.folderCount || c.section(kDRecs).count != i.recordCount) return Why::Counts;
  if (i.folderCount == 0) return Why::FoldRoot;
  if (i.recordCount > 0 && !c.section(kDHidx).present()) return Why::Missing;
  // DHDR, whole.
  const cc::Section& dh = c.section(kDDhdr);
  uint8_t h[kDhdrBytes];
  if (dh.bytes < kDhdrBytes) return Why::Shape;
  if (c.checkCrc(kDDhdr, h, sizeof(h)) != Why::Ok) return Why::SectionCrc;
  if (!src.read(dh.offset, h, kDhdrBytes)) return Why::Io;
  if (!decodeHeader(h, dh.bytes, &out->header)) return Why::Enum;
  // DSTA's and DFLD's headers.
  const cc::Section& ds = c.section(kDDsta);
  if (ds.bytes != kDstaHeader + i.recordCount) return Why::Shape;
  uint8_t b[kDstaHeader];
  if (!src.read(ds.offset, b, kDstaHeader)) return Why::Io;
  if (cc::get32(b) != i.recordCount) return Why::Counts;
  out->ownRecords = cc::get32(b + 4);
  out->ownFolders = cc::get32(b + 8);
  if (out->ownRecords > i.recordCount || out->ownFolders > i.folderCount) return Why::Counts;
  const cc::Section& df = c.section(kDDfld);
  if (df.bytes < kDfldHeader + kFactsFixed * i.folderCount) return Why::Shape;
  if (!src.read(df.offset, b, kDfldHeader)) return Why::Io;
  if (cc::get32(b) != i.folderCount) return Why::Counts;
  out->present = true;
  out->headerCrc = i.frame.headerCrc;
  out->generation = i.frame.generation;
  return Why::Ok;
}

}  // namespace

Why openDevice(cc::Source& src, DeviceInfo* out) {
  cc::Container c;
  return openDev(c, src, out);
}

// ---------------------------------------------------------------------------
// DeviceReader
// ---------------------------------------------------------------------------
DeviceReader::Step DeviceReader::fail(Why w) {
  if (why_ == Why::Ok) why_ = w;
  return Step::Bad;
}

Why DeviceReader::begin(cc::Source& src, uint8_t* scratch, uint32_t scratchBytes, cc::RunFields* run, bool facts) {
  src_ = &src;
  why_ = Why::Ok;
  factsOn_ = facts;
  facts_ = FolderFacts();
  row_ = Row();
  if (!scratch || scratchBytes < kMinScratch) return why_ = Why::Io;
  why_ = openDev(c_, src, &info_);
  if (why_ != Why::Ok) return why_;
  // DSTA's and DFLD's streams an eighth each (64-512 bytes), the walker
  // never below its own minimum.
  uint32_t part = scratchBytes / 8;
  if (part < 64) part = 64;
  if (part > 512) part = 512;
  const uint32_t spare = (scratchBytes - mptg::Walker::kMinScratch) / 2;
  if (part > spare) part = spare;
  const uint32_t walker = scratchBytes - 2 * part;
  why_ = w_.begin(src, 0, scratch, walker, run);
  if (why_ != Why::Ok) return why_;
  const cc::Section& ds = c_.section(kDDsta);
  const cc::Section& df = c_.section(kDDfld);
  dsta_.begin(&src, ds.offset, ds.offset + ds.bytes, scratch + walker, part);
  dfld_.begin(&src, df.offset, df.offset + df.bytes, scratch + walker + part, part);
  if (!dsta_.skipTo(ds.offset + kDstaHeader)) return why_ = Why::Io;
  if (factsOn_ && !dfld_.skipTo(df.offset + kDfldHeader)) return why_ = Why::Io;
  return why_;
}

DeviceReader::Step DeviceReader::next() {
  if (why_ != Why::Ok) return Step::Bad;
  const mptg::Walker::Step s = w_.next();
  switch (s) {
    case mptg::Walker::Step::Folder:
      if (factsOn_ && !readFacts(dfld_, &facts_)) return fail(dfld_.failed() ? Why::Io : Why::Shape);
      return Step::Folder;
    case mptg::Walker::Step::Record: {
      uint8_t b = 0;
      if (!dsta_.readByte(&b)) return fail(Why::Io);
      if (b & ~7u) return fail(Why::Enum);
      row_ = decodeRow(b);
      return Step::Record;
    }
    case mptg::Walker::Step::End: {
      if (dsta_.position() != dsta_.end()) return fail(Why::Shape);
      if (!dsta_.drain() || dsta_.crc() != c_.section(kDDsta).crc) return fail(Why::SectionCrc);
      if (factsOn_) {
        if (dfld_.position() != dfld_.end()) return fail(Why::Shape);
        if (!dfld_.drain() || dfld_.crc() != c_.section(kDDfld).crc) return fail(Why::SectionCrc);
      }
      return Step::End;
    }
    case mptg::Walker::Step::Bad:
      break;
  }
  return fail(w_.why());
}

// ---------------------------------------------------------------------------
// FolderCursor
// ---------------------------------------------------------------------------
bool FolderCursor::begin(cc::Source& src, uint8_t* buf, uint32_t bufBytes) {
  src_ = &src;
  buf_ = buf;
  bufBytes_ = bufBytes;
  failed_ = true;
  have_ = false;
  next_ = 0;
  depth_ = 0;
  pathLen_ = 0;
  path_[0] = 0;
  lastLen_ = 0;
  last_[0] = 0;
  asked_ = false;
  if (!buf || bufBytes < 3 * 64) return false;
  DeviceInfo info;
  if (openDev(c_, src, &info) != Why::Ok) return false;
  folders_ = info.tags.folderCount;
  const cc::Section& fold = c_.section(kDFold);
  const cc::Section& recs = c_.section(kDRecs);
  const cc::Section& strs = c_.section(kDStrs);
  const cc::Section& dfld = c_.section(kDDfld);
  split_ = strs.bytes;
  if (info.tags.recordCount > 0) {
    uint8_t row[mptg::kRecsStride];
    if (!src.read(recs.offset, row, mptg::kRecsStride)) return false;
    mptg::Record r;
    mptg::decodeRecord(row, &r);
    if (r.name == 0 || r.name > strs.bytes) return false;
    split_ = r.name;
  }
  const uint32_t part = bufBytes / 3;
  fold_.begin(&src, fold.offset, fold.offset + fold.bytes, buf, part);
  names_.begin(&src, strs.offset, strs.offset + split_, buf + part, part);
  dfld_.begin(&src, dfld.offset, dfld.offset + dfld.bytes, buf + 2 * part, part);
  if (!dfld_.skipTo(dfld.offset + kDfldHeader)) return false;
  failed_ = false;
  return true;
}

bool FolderCursor::advance() {
  if (failed_ || next_ >= folders_) return false;
  const cc::Section& fold = c_.section(kDFold);
  const cc::Section& strs = c_.section(kDStrs);
  uint8_t row[mptg::kFoldStride];
  if (!fold_.read(row, mptg::kFoldStride) || !fold_.skipTo(fold_.position() + fold.stride - mptg::kFoldStride)) {
    failed_ = true;
    return false;
  }
  mptg::Folder f;
  mptg::decodeFolder(row, &f);
  const uint32_t i = next_++;
  if (i == 0) {
    depth_ = 1;
    stack_[0] = Level{0, 0};
    pathLen_ = 0;
  } else {
    while (depth_ > 0 && stack_[depth_ - 1].index != f.parent) --depth_;
    char name[cc::kMaxRelPath + 1];
    size_t len = 0;
    if (depth_ == 0 || depth_ > mptg::Walker::kMaxDepth || f.name == 0 || f.name >= split_ ||
        strs.offset + f.name < names_.position() || !names_.skipTo(strs.offset + f.name) ||
        !names_.readString(name, sizeof(name), &len) || len == 0 || len > cc::kMaxRelPath) {
      failed_ = true;
      return false;
    }
    const size_t pe = stack_[depth_ - 1].end;
    const size_t at = pe ? pe + 1 : 0;
    if (at + len > cc::kMaxRelPath) {
      failed_ = true;
      return false;
    }
    if (pe) path_[pe] = '/';
    std::memcpy(path_ + at, name, len);
    pathLen_ = at + len;
    stack_[depth_++] = Level{i, static_cast<uint16_t>(pathLen_)};
  }
  path_[pathLen_] = 0;
  if (!readFacts(dfld_, &cur_)) {
    failed_ = true;
    return false;
  }
  have_ = true;
  return true;
}

bool FolderCursor::find(const char* rel, size_t len, FolderFacts* out) {
  if (asked_ && cc::compareFolderPaths(rel, len, last_, lastLen_) < 0) {
    // Asked again from an earlier folder (a build that restarted).
    if (!begin(*src_, buf_, bufBytes_)) return false;
  }
  asked_ = true;
  if (len <= cc::kMaxRelPath) {
    std::memcpy(last_, rel, len);
    last_[len] = 0;
    lastLen_ = len;
  }
  if (failed_) return false;
  while (!have_ || cc::compareFolderPaths(path_, pathLen_, rel, len) < 0)
    if (!advance()) return false;
  if (cc::compareFolderPaths(path_, pathLen_, rel, len) != 0) return false;
  *out = cur_;
  return true;
}

bool FolderCursor::finish() {
  while (!failed_ && next_ < folders_) advance();
  if (failed_) return false;
  if (dfld_.position() != dfld_.end()) return false;
  return dfld_.drain() && dfld_.crc() == c_.section(kDDfld).crc;
}

// ---------------------------------------------------------------------------
// The builder's adapters
// ---------------------------------------------------------------------------
bool BuilderRows::begin(cc::Source& src, uint8_t* buf, uint32_t bufBytes) {
  src_ = &src;
  buf_ = buf;
  bufBytes_ = bufBytes;
  ok_ = false;
  next_ = 0;
  cc::Container c;
  if (!buf || bufBytes == 0 || openDev(c, src, &info_) != Why::Ok) return false;
  dsta_ = c.section(kDDsta);
  // DSTA whole: the builder's walker doesn't know the device's sections.
  if (c.checkCrc(kDDsta, buf, bufBytes) != Why::Ok) return false;
  s_.begin(&src, dsta_.offset, dsta_.offset + dsta_.bytes, buf, bufBytes);
  ok_ = s_.skipTo(dsta_.offset + kDstaHeader);
  return ok_;
}

LibraryBuilder::Row BuilderRows::row(uint32_t record) {
  LibraryBuilder::Row pending;
  pending.status = Status::Pending;
  if (!ok_) return pending;
  if (record < next_) {
    s_.begin(src_, dsta_.offset, dsta_.offset + dsta_.bytes, buf_, bufBytes_);
    next_ = 0;
    if (!s_.skipTo(dsta_.offset + kDstaHeader)) {
      ok_ = false;
      return pending;
    }
  }
  uint8_t b = 0;
  while (next_ <= record) {
    if (!s_.readByte(&b)) {
      ok_ = false;
      return pending;
    }
    ++next_;
  }
  if (b & ~7u) return pending;
  return builderRow(decodeRow(b));
}

bool BuilderRows::ownCounts(uint32_t* records, uint32_t* folders) {
  if (!ok_) return false;
  *records = info_.ownRecords;
  *folders = info_.ownFolders;
  return true;
}

bool BuilderFacts::facts(const char* rel, size_t len, LibraryIndex::FolderFacts* out) {
  if (!cursor_.find(rel, len, &f_)) return false;
  *out = f_.index();
  return true;
}

// ---------------------------------------------------------------------------
// ChunkBuilder
// ---------------------------------------------------------------------------
void ChunkBuilder::begin(uint8_t* buf, uint32_t bytes) {
  buf_ = buf;
  // The offsets at the back are u32s: their end aligned.
  const uintptr_t end = (reinterpret_cast<uintptr_t>(buf) + bytes) & ~static_cast<uintptr_t>(3);
  cap_ = buf ? static_cast<uint32_t>(end - reinterpret_cast<uintptr_t>(buf)) : 0;
  used_ = count_ = 0;
}

bool ChunkBuilder::add(const char* rel, size_t len, Status status, const mptg::Record& rec,
                       const char* const fields[cc::kRunFields]) {
  if (!buf_ || !rel || !cc::validRelPath(rel, len)) return false;
  if (status != Status::Scanned && status != Status::Unreadable) return false;
  const uint32_t head = kEntryHead + static_cast<uint32_t>(len) + mptg::kRecsStride + 2;
  if (cap_ < used_ + 4 * count_ + head + 4) return false;
  const uint32_t room = cap_ - used_ - 4 * count_;  // the entry and its offset go here
  uint8_t* p = buf_ + used_;
  bool any = false;
  if (fields)
    for (uint32_t f = 0; f < cc::kRunFields; ++f)
      if (fields[f] && fields[f][0]) any = true;
  size_t runLen = 0;
  if (any && status == Status::Scanned) {
    runLen = cc::encodeRun(fields, p + head, room - head - 4);
    if (runLen == 0 || runLen > 0xFFFF) return false;  // no room
  }
  p[0] = static_cast<uint8_t>(EntryKind::Record);
  p[1] = encodeRow(Row{status, false});
  cc::put16(p + 2, static_cast<uint16_t>(len));
  std::memcpy(p + kEntryHead, rel, len);
  mptg::Record r = rec;
  r.folder = r.name = r.strings = 0;
  if (status == Status::Unreadable) r.flags |= mptg::kUnreadable;
  mptg::encodeRecord(r, p + kEntryHead + len);
  cc::put16(p + kEntryHead + len + mptg::kRecsStride, static_cast<uint16_t>(runLen));
  const uint32_t at = used_;
  used_ += head + static_cast<uint32_t>(runLen);
  ++count_;
  std::memcpy(buf_ + cap_ - 4 * count_, &at, 4);
  return true;
}

// ---------------------------------------------------------------------------
// The merge: D, walk.jnl's runs and tags.jnl's chunks as one stream of rows
// in canonical order (the compaction's and View's).
// ---------------------------------------------------------------------------
namespace {

enum InputType : uint8_t { kInBin = 0, kInWalk, kInChunk };

struct Input {
  uint8_t type = kInChunk;
  bool has = false;
  bool done = false;
  bool advance = false;  // in the row just given: moved past at the next step
  bool convert = false;  // a chunk of another parser or epoch: its records turn Pending
  char path[cc::kMaxRelPath + 1];
  uint16_t len = 0;
  uint8_t kind = 0, flags = 0;
  mptg::Record rec;
  uint32_t size = 0, fatTime = 0;
  uint64_t qfp = 0;
  uint16_t runLen = 0;
  bool runPending = false;  // the run's bytes are next in the stream
  cc::Stream s;
  uint32_t left = 0;     // a chunk's entries left; a walk run's in its block
  bool inBlock = false;  // a walk run: inside a block (its CRC follows its last entry)
};

// A walk run's next entry: 1 at an entry (read whole), 0 the run's end, -1
// failed. `folders`: its Folder entries only (`facts` receives each one's),
// else its File and Gone entries only.
int nextWalkEntry(Input& in, bool folders, FolderFacts* facts) {
  for (;;) {
    if (in.inBlock && in.left == 0) {
      uint8_t crc[4];
      if (!in.s.read(crc, 4)) return -1;
      in.inBlock = false;
    }
    if (!in.inBlock) {
      uint8_t h[kBlockHeader];
      if (!in.s.read(h, kBlockHeader)) return -1;
      if (cc::get32(h) != kBlockMagic) return -1;
      if (h[5] == kBlockEnd) {
        uint8_t rest[kEndPayload + 4];
        if (!in.s.read(rest, sizeof(rest))) return -1;
        return 0;
      }
      in.left = cc::get32(h + 8);
      in.inBlock = true;
      continue;
    }
    uint8_t h[kEntryHead];
    if (!in.s.read(h, kEntryHead)) return -1;
    --in.left;
    in.kind = h[0];
    in.flags = h[1];
    in.len = cc::get16(h + 2);
    if (in.len > cc::kMaxRelPath || !in.s.read(in.path, in.len)) return -1;
    in.path[in.len] = 0;
    uint8_t t[kDoubtTail];
    bool want = false;
    switch (static_cast<EntryKind>(in.kind)) {
      case EntryKind::File:
        if (!in.s.read(t, kFileTail)) return -1;
        in.size = cc::get32(t);
        in.fatTime = cc::get32(t + 4);
        in.qfp = cc::get64(t + 8);
        want = !folders;
        break;
      case EntryKind::Gone:
        want = !folders;
        break;
      case EntryKind::Doubt:
        if (!in.s.read(t, kDoubtTail)) return -1;
        break;
      case EntryKind::Folder: {
        FolderFacts tmp;
        if (!readFacts(in.s, facts && folders ? facts : &tmp)) return -1;
        want = folders;
        break;
      }
      case EntryKind::FolderGone:
        break;
      default:
        return -1;
    }
    if (want) return 1;
  }
}

// A chunk's next record head (the run's bytes left in the stream).
int nextChunkEntry(Input& in) {
  if (in.runPending) {
    if (!in.s.skipTo(in.s.position() + in.runLen)) return -1;
    in.runPending = false;
  }
  if (in.left == 0) return 0;
  uint8_t h[kEntryHead];
  if (!in.s.read(h, kEntryHead)) return -1;
  --in.left;
  in.kind = h[0];
  in.flags = h[1];
  in.len = cc::get16(h + 2);
  if (in.kind != static_cast<uint8_t>(EntryKind::Record) || in.len > cc::kMaxRelPath || !in.s.read(in.path, in.len))
    return -1;
  in.path[in.len] = 0;
  uint8_t r[mptg::kRecsStride + 2];
  if (!in.s.read(r, sizeof(r))) return -1;
  mptg::decodeRecord(r, &in.rec);
  in.runLen = cc::get16(r + mptg::kRecsStride);
  in.runPending = in.runLen > 0;
  return 1;
}

// A walk run's folder entries, looked up by path in pre-order.
struct WalkFolders {
  Input in;
  bool on = false;
  bool have = false;
  FolderFacts cur;
  void begin(cc::Source* src, uint32_t start, uint32_t end, uint8_t* buf, uint32_t bytes) {
    in = Input();
    in.type = kInWalk;
    in.s.begin(src, start, end, buf, bytes);
    on = true;
    have = false;
  }
  bool find(const char* rel, size_t len, FolderFacts* out) {
    if (!on) return false;
    while (!have || cc::compareFolderPaths(in.path, in.len, rel, len) < 0) {
      const int r = nextWalkEntry(in, true, &cur);
      if (r != 1) {
        on = false;  // the run's end, or a failed read: no more facts from it
        return false;
      }
      have = true;
    }
    if (cc::compareFolderPaths(in.path, in.len, rel, len) != 0) return false;
    *out = cur;
    return true;
  }
};

}  // namespace

struct Merge {
  Fs* fs = nullptr;
  File* binF = nullptr;
  File* walkF = nullptr;
  File* jnlF = nullptr;
  // D
  bool useBin = false;
  bool binConvert = false;  // D read by another parser, or at another epoch
  bool binEnded = false;
  DeviceReader* dev = nullptr;
  cc::RunFields* binRun = nullptr;
  // The inputs, in rank order (the newest last).
  Input* in = nullptr;
  uint32_t n = 0;
  // The row given.
  bool present = false;
  Row row;
  mptg::Record rec;
  int32_t runFrom = -1;  // the input whose run the row carries; -1 none
  const char* path = nullptr;
  uint16_t len = 0;
  bool failed = false;
  bool binFailed = false;  // D failed its checks (Why in dev)
  bool converted = false;  // some row was turned Pending by another parser or epoch

  void closeFiles() {
    if (binF) fs->close(binF);
    if (walkF) fs->close(walkF);
    if (jnlF) fs->close(jnlF);
    binF = walkF = jnlF = nullptr;
  }

  // The bin's next record.
  void advanceBin(Input& i) {
    for (;;) {
      const DeviceReader::Step s = dev->next();
      if (s == DeviceReader::Step::Folder) continue;
      if (s == DeviceReader::Step::Record) {
        i.has = true;
        const mptg::Walker& w = dev->walker();
        i.len = static_cast<uint16_t>(w.pathLength());
        std::memcpy(i.path, w.path(), i.len);
        i.path[i.len] = 0;
        return;
      }
      i.has = false;
      i.done = true;
      if (s == DeviceReader::Step::End) {
        binEnded = true;
      } else {
        binFailed = true;
        failed = true;
      }
      return;
    }
  }

  void advanceInput(Input& i) {
    i.advance = false;
    if (i.done) return;
    int r;
    if (i.type == kInBin) {
      advanceBin(i);
      return;
    }
    r = i.type == kInWalk ? nextWalkEntry(i, false, nullptr) : nextChunkEntry(i);
    if (r == 1) {
      i.has = true;
    } else {
      i.has = false;
      i.done = true;
      if (r < 0) failed = true;
    }
  }

  // The run length of the bin's record as written again (from its fields).
  uint32_t binRunLength() const {
    const cc::RunFields* f = dev->walker().run();
    if (!f) return 0;
    int last = -1;
    for (uint32_t k = 0; k < cc::kRunFields; ++k)
      if (f->has(k)) last = static_cast<int>(k);
    if (last < 0) return 0;
    uint32_t n = 1;
    for (int k = 0; k <= last; ++k) n += static_cast<uint32_t>(f->len(static_cast<uint32_t>(k))) + 1;
    return n;
  }

  uint32_t runLength() const {
    if (runFrom < 0) return 0;
    const Input& i = in[runFrom];
    return i.type == kInBin ? binRunLength() : i.runLen;
  }

  // The row's run, to `out` (a counting cursor in the first pass), its
  // values to the distinct counts.
  bool copyRun(Cursor* out, ValueHasher* hasher) {
    if (runFrom < 0) return true;
    Input& i = in[runFrom];
    if (i.type == kInBin) {
      const cc::RunFields* f = dev->walker().run();
      int last = -1;
      for (uint32_t k = 0; k < cc::kRunFields; ++k)
        if (f->has(k)) last = static_cast<int>(k);
      if (last < 0) return true;
      const uint8_t nb = static_cast<uint8_t>(last + 1);
      if (hasher) hasher->begin();
      if (out && !out->put(&nb, 1)) return false;
      if (hasher) hasher->feed(&nb, 1);
      for (int k = 0; k <= last; ++k) {
        const uint8_t* s = reinterpret_cast<const uint8_t*>(f->get(static_cast<uint32_t>(k)));
        const uint32_t l = static_cast<uint32_t>(f->len(static_cast<uint32_t>(k))) + 1;  // with its NUL
        if (out && !out->put(s, l)) return false;
        if (hasher) hasher->feed(s, l);
      }
      return true;
    }
    if (!i.runPending) return false;
    uint8_t b[128];
    uint32_t left = i.runLen;
    if (hasher) hasher->begin();
    while (left) {
      const uint32_t k = left < sizeof(b) ? left : static_cast<uint32_t>(sizeof(b));
      if (!i.s.read(b, k)) return false;
      if (out && !out->put(b, k)) return false;
      if (hasher) hasher->feed(b, k);
      left -= k;
    }
    i.runPending = false;
    return true;
  }

  // One source's say on the path, over the row so far (rank order).
  void apply(int32_t k) {
    Input& i = in[k];
    if (i.type == kInBin) {
      const Row r = dev->row();
      const mptg::Record& d = dev->walker().record();
      present = true;
      row = r;
      if ((r.status == Status::Scanned || r.status == Status::Unreadable) && binConvert) {
        row.status = Status::Pending;
        row.confirmed = false;
        rec = bare(d.size, d.fatTime, d.qfp);
        runFrom = -1;
        converted = true;
      } else if (r.status == Status::Software || r.status == Status::Pending) {
        rec = bare(d.size, d.fatTime, d.qfp);
        runFrom = -1;
      } else {
        rec = d;
        runFrom = r.status == Status::Scanned && binRunLength() > 0 ? k : -1;
      }
      return;
    }
    if (i.type == kInWalk) {
      if (static_cast<EntryKind>(i.kind) != EntryKind::File) {  // Gone (the walk inputs give no other kind)
        present = false;
        runFrom = -1;
        return;
      }
      const Row w = decodeRow(i.flags);
      // The device's reading of the file at this size and time stands, the
      // walk's qfp taken, unless T's record now covers the file.
      const bool reading = present && (row.status == Status::Scanned || row.status == Status::Unreadable) &&
                           rec.size == i.size && rec.fatTime == i.fatTime;
      if (w.status != Status::Software && reading) {
        if (i.qfp) rec.qfp = i.qfp;
        return;
      }
      // A qfp of the file at this size and time carries over.
      const uint64_t q = i.qfp ? i.qfp : (present && rec.size == i.size && rec.fatTime == i.fatTime ? rec.qfp : 0);
      present = true;
      row = w;
      // A reading the walk names that isn't here to keep: the scan reads it.
      if (row.status == Status::Scanned || row.status == Status::Unreadable) row = Row{Status::Pending, false};
      rec = bare(i.size, i.fatTime, q);
      runFrom = -1;
      return;
    }
    // A chunk's record.
    present = true;
    row = decodeRow(i.flags);
    row.confirmed = false;
    if (i.convert) {
      row.status = Status::Pending;
      rec = bare(i.rec.size, i.rec.fatTime, i.rec.qfp);
      runFrom = -1;
      converted = true;
    } else {
      rec = i.rec;
      runFrom = row.status == Status::Scanned && i.runLen > 0 ? k : -1;
    }
  }

  // The next row; false at the end (or failed).
  bool next() {
    for (;;) {
      if (failed) return false;
      for (uint32_t k = 0; k < n; ++k)
        if (in[k].advance) advanceInput(in[k]);
      if (failed) return false;
      int32_t min = -1;
      for (uint32_t k = 0; k < n; ++k) {
        if (!in[k].has) continue;
        if (min < 0 || cc::compareFilePaths(in[k].path, in[k].len, in[min].path, in[min].len) < 0)
          min = static_cast<int32_t>(k);
      }
      if (min < 0) return false;
      present = false;
      runFrom = -1;
      const Input& m = in[min];
      for (uint32_t k = 0; k < n; ++k) {
        Input& i = in[k];
        if (!i.has || i.len != m.len || std::memcmp(i.path, m.path, m.len) != 0) continue;
        apply(static_cast<int32_t>(k));
        i.advance = true;
      }
      path = m.path;
      len = m.len;
      if (present) return true;
    }
  }
};

// ---------------------------------------------------------------------------
// TagStore
// ---------------------------------------------------------------------------
namespace {

// A bump allocator over one block.
struct Bump {
  uint8_t* p;
  size_t left;
  void* take(size_t n) {
    n = (n + 7) & ~static_cast<size_t>(7);
    if (n > left) return nullptr;
    void* r = p;
    p += n;
    left -= n;
    return r;
  }
};

// The merge's arena, as beginMerge() takes it: the inputs (D's, two walk
// runs', the chunks'), D's reader, its run and buffers, each run's buffer;
// after the merge, HIDX's sort. Each piece rounded up to 8.
size_t arenaBytes(const TagStore::Config& c, uint32_t chunks) {
  const size_t runs = 2 + static_cast<size_t>(chunks);
  const size_t merge = sizeof(Input) * (runs + 1) + sizeof(DeviceReader) + sizeof(cc::RunFields) + c.deviceBuffer +
                       runs * c.runBuffer + 8 * (runs + 4);
  const size_t hidx = 16384 + c.runBuffer;
  return merge > hidx ? merge : hidx;
}

}  // namespace

TagStore::TagStore(Fs& fs, const Config& config, AllocFn alloc, FreeFn release)
    : fs_(fs), cfg_(config), alloc_(alloc ? alloc : defaultAlloc), free_(release ? release : defaultFree) {
  if (cfg_.runBuffer < 64) cfg_.runBuffer = 64;
  if (cfg_.writeBuffer < 64) cfg_.writeBuffer = 64;
  if (cfg_.deviceBuffer < DeviceReader::kMinScratch) cfg_.deviceBuffer = DeviceReader::kMinScratch;
  if (cfg_.maxChunks < 1) cfg_.maxChunks = 1;
  const char* d = cfg_.dir ? cfg_.dir : "";
  std::snprintf(binPath_, sizeof(binPath_), "%s/tags.bin", d);
  std::snprintf(tmpPath_, sizeof(tmpPath_), "%s/tags.tmp", d);
  std::snprintf(stemPath_, sizeof(stemPath_), "%s/tags", d);
  std::snprintf(jnlPath_, sizeof(jnlPath_), "%s/tags.jnl", d);
  std::snprintf(walkPath_, sizeof(walkPath_), "%s/walk.jnl", d);
  std::snprintf(hidxPath_, sizeof(hidxPath_), "%s/hidx.tmp", d);
  names_.path = binPath_;
  names_.tmp = tmpPath_;
  names_.stem = stemPath_;
  chunks_ = static_cast<Chunk*>(alloc_(sizeof(Chunk) * cfg_.maxChunks));
}

TagStore::~TagStore() {
  if (chunks_) free_(chunks_);
}

void TagStore::release(void* p) {
  if (p) free_(p);
}

bool TagStore::wantsCompaction() const {
  if (chunkN_ >= cfg_.maxChunks || jnlEnd_ >= cfg_.compactBytes) return true;
  return dev_.present && (dev_.tags.parserVersion != cfg_.parserVersion || dev_.tags.readRules != mptg::kReadRules);
}

// D whole (tags.tmp before it is promoted): every check a reader makes.
namespace {

class DeviceCheck : public TmpCheck {
public:
  DeviceCheck(TagStore::Config c, void* (*alloc)(size_t), void (*release)(void*))
      : cfg_(c), alloc_(alloc), free_(release) {}
  bool whole(Fs& fs, const char* tmp) override {
    File* f = fs.open(tmp, Fs::Mode::Read);
    if (!f) return false;
    const size_t bytes = sizeof(DeviceReader) + cfg_.deviceBuffer + 16;
    void* mem = alloc_(bytes);
    bool ok = false;
    if (mem) {
      Bump b{static_cast<uint8_t*>(mem), bytes};
      // (Zeroed, then default-initialized in place: compact()'s note.)
      void* rm = b.take(sizeof(DeviceReader));
      std::memset(rm, 0, sizeof(DeviceReader));
      DeviceReader* r = new (rm) DeviceReader;
      uint8_t* scratch = static_cast<uint8_t*>(b.take(cfg_.deviceBuffer));
      if (r->begin(*f, scratch, cfg_.deviceBuffer, nullptr, true) == Why::Ok) {
        DeviceReader::Step s;
        while ((s = r->next()) == DeviceReader::Step::Folder || s == DeviceReader::Step::Record) {
        }
        ok = s == DeviceReader::Step::End;
      }
      r->~DeviceReader();
      free_(mem);
    }
    fs.close(f);
    return ok;
  }

private:
  TagStore::Config cfg_;
  void* (*alloc_)(size_t);
  void (*free_)(void*);
};

}  // namespace

bool TagStore::loadDevice() {
  dev_ = DeviceInfo();
  File* f = fs_.open(binPath_, Fs::Mode::Read);
  if (!f) return false;
  const Why w = openDevice(*f, &dev_);
  fs_.close(f);
  if (w != Why::Ok) dev_ = DeviceInfo();
  return w == Why::Ok;
}

TagStore::Opened TagStore::open() {
  Opened o;
  walking_ = false;
  DeviceCheck check(cfg_, alloc_, free_);
  o.settled = settle(fs_, names_, &check);
  twins_ = o.settled.twins;
  if (fs_.exists(hidxPath_)) fs_.remove(hidxPath_);
  dev_ = DeviceInfo();
  if (fs_.exists(binPath_)) {
    File* f = fs_.open(binPath_, Fs::Mode::Read);
    if (f) {
      o.deviceWhy = openDevice(*f, &dev_);
      fs_.close(f);
      if (o.deviceWhy != Why::Ok) dev_ = DeviceInfo();
    } else {
      o.deviceWhy = Why::Io;
    }
  } else {
    o.deviceWhy = Why::Missing;
  }
  scanJournal(&o);
  scanWalk(&o);
  return o;
}

// ---------------------------------------------------------------------------
// tags.jnl
// ---------------------------------------------------------------------------
namespace {

// Checks one chunk's entries in a stream over its payload: Record entries in
// strictly increasing canonical order, each run well formed.
bool checkChunkEntries(cc::Stream& s, uint32_t count, char* prev, char* cur) {
  size_t prevLen = 0;
  for (uint32_t e = 0; e < count; ++e) {
    uint8_t h[kEntryHead];
    if (!s.read(h, kEntryHead)) return false;
    const uint16_t len = cc::get16(h + 2);
    if (h[0] != static_cast<uint8_t>(EntryKind::Record) || !scanStatus(h[1]) || len > cc::kMaxRelPath) return false;
    if (!s.read(cur, len)) return false;
    cur[len] = 0;
    if (!cc::validRelPath(cur, len)) return false;
    if (e > 0 && cc::compareFilePaths(prev, prevLen, cur, len) >= 0) return false;
    uint8_t r[mptg::kRecsStride + 2];
    if (!s.read(r, sizeof(r))) return false;
    const uint16_t runLen = cc::get16(r + mptg::kRecsStride);
    if (runLen > cc::kRunMax) return false;
    if (runLen) {
      ValueHasher v;
      v.begin();
      uint8_t b[64];
      uint32_t left = runLen;
      while (left) {
        const uint32_t k = left < sizeof(b) ? left : static_cast<uint32_t>(sizeof(b));
        if (!s.read(b, k) || !v.feed(b, k)) return false;
        left -= k;
      }
      if (!v.complete()) return false;
    }
    std::memcpy(prev, cur, len + 1u);
    prevLen = len;
  }
  return true;
}

}  // namespace

void TagStore::scanJournal(Opened* o) {
  chunkN_ = 0;
  extraChunks_ = 0;
  jnlEnd_ = 0;
  jnlTail_ = false;
  jnlForeign_ = false;
  File* f = fs_.open(jnlPath_, Fs::Mode::Read);
  if (!f) return;
  const uint32_t size = f->size();
  void* mem = alloc_(cfg_.runBuffer + 2 * (cc::kMaxRelPath + 1));
  if (!mem || !chunks_) {
    if (mem) free_(mem);
    fs_.close(f);
    jnlForeign_ = size > 0;
    return;
  }
  uint8_t* buf = static_cast<uint8_t*>(mem);
  char* prev = reinterpret_cast<char*>(buf + cfg_.runBuffer);
  char* cur = prev + cc::kMaxRelPath + 1;
  const uint32_t base = deviceCrc();
  uint32_t at = 0;
  uint32_t extra = 0;
  for (;;) {
    if (size - at < kChunkHeader + kChunkTrailer) break;
    uint8_t h[kChunkHeader];
    if (!f->read(at, h, kChunkHeader)) break;
    const uint32_t seq = cc::get32(h + 4), cbase = cc::get32(h + 8), count = cc::get32(h + 12);
    const uint32_t payload = cc::get32(h + 16);
    if (cc::get32(h) != kJnlMagic) break;
    if (cbase != base) {
      if (at == 0) o->journalStale = true;
      break;
    }
    if (seq != chunkN_ + extra + 1) break;
    if (payload > size - at - kChunkHeader - kChunkTrailer) break;
    cc::Stream s;
    s.begin(f, at, at + kChunkHeader + payload, buf, cfg_.runBuffer);
    if (!s.skipTo(at + kChunkHeader) || !checkChunkEntries(s, count, prev, cur) || s.position() != s.end()) break;
    uint8_t c[4];
    if (!f->read(at + kChunkHeader + payload, c, 4) || cc::get32(c) != s.crc()) break;
    const uint32_t bytes = kChunkHeader + payload + kChunkTrailer;
    if (chunkN_ < cfg_.maxChunks) {
      Chunk& k = chunks_[chunkN_++];
      k.offset = at;
      k.bytes = bytes;
      k.seq = seq;
      k.count = count;
      k.parser = cc::get16(h + 20);
      k.rules = cc::get16(h + 22);
      k.epoch = cc::get32(h + 24);
      jnlEnd_ = at + bytes;
    } else {
      ++extra;  // past maxChunks: dropped at the compaction
    }
    at += bytes;
  }
  extraChunks_ = extra;
  free_(mem);
  fs_.close(f);
  jnlForeign_ = chunkN_ == 0 && size > 0;
  jnlTail_ = chunkN_ > 0 && size > jnlEnd_;
  o->chunks = chunkN_;
  o->journalTorn = jnlTail_ || (jnlForeign_ && !o->journalStale);
}

bool TagStore::append(ChunkBuilder& c) {
  if (c.count_ == 0) return true;
  if (!chunks_ || chunkN_ >= cfg_.maxChunks || extraChunks_ > 0 || walking_) return false;
  // Sort the offsets by path, then by when they were added (an offset grows
  // with each add): a path added twice keeps its last.
  uint32_t* idx = reinterpret_cast<uint32_t*>(c.buf_ + c.cap_ - 4 * c.count_);
  const uint8_t* buf = c.buf_;
  auto pathOf = [buf](uint32_t off, size_t* len) {
    *len = cc::get16(buf + off + 2);
    return reinterpret_cast<const char*>(buf + off + kEntryHead);
  };
  std::sort(idx, idx + c.count_, [&](uint32_t a, uint32_t b) {
    size_t al, bl;
    const char* ap = pathOf(a, &al);
    const char* bp = pathOf(b, &bl);
    const int r = cc::compareFilePaths(ap, al, bp, bl);
    return r ? r < 0 : a < b;
  });
  auto entryBytes = [buf](uint32_t off) {
    const uint32_t len = cc::get16(buf + off + 2);
    const uint32_t runLen = cc::get16(buf + off + kEntryHead + len + mptg::kRecsStride);
    return kEntryHead + len + mptg::kRecsStride + 2 + runLen;
  };
  auto keep = [&](uint32_t k) {
    if (k + 1 >= c.count_) return true;
    size_t al, bl;
    const char* ap = pathOf(idx[k], &al);
    const char* bp = pathOf(idx[k + 1], &bl);
    return !(al == bl && std::memcmp(ap, bp, al) == 0);
  };
  uint32_t count = 0, payload = 0;
  for (uint32_t k = 0; k < c.count_; ++k)
    if (keep(k)) {
      ++count;
      payload += entryBytes(idx[k]);
    }

  uint8_t* wbuf = static_cast<uint8_t*>(alloc_(cfg_.writeBuffer));
  if (!wbuf) return false;
  const bool fresh = chunkN_ == 0;
  File* f = fs_.open(jnlPath_, fresh ? Fs::Mode::Create : Fs::Mode::Update);
  bool ok = f != nullptr;
  const uint32_t at = fresh ? 0 : jnlEnd_;
  if (ok && !fresh && jnlTail_) ok = f->truncate(jnlEnd_);
  uint8_t h[kChunkHeader] = {};
  cc::put32(h, kJnlMagic);
  cc::put32(h + 4, journalSeq() + 1);
  cc::put32(h + 8, deviceCrc());
  cc::put32(h + 12, count);
  cc::put32(h + 16, payload);
  cc::put16(h + 20, cfg_.parserVersion);
  cc::put16(h + 22, mptg::kReadRules);
  cc::put32(h + 24, dev_.present ? dev_.header.epoch : 0);
  Cursor w;
  w.begin(f, at, wbuf, cfg_.writeBuffer);
  if (ok) ok = w.put(h, kChunkHeader);
  for (uint32_t k = 0; ok && k < c.count_; ++k)
    if (keep(k)) ok = w.put(buf + idx[k], entryBytes(idx[k]));
  uint8_t crc[4];
  cc::put32(crc, w.crc);
  if (ok) ok = w.put(crc, 4) && w.flush() && f->sync();
  if (f) ok = fs_.close(f) && ok;
  free_(wbuf);
  if (!ok) {
    // Whatever reached the card is a torn tail: cut off at the next append.
    if (fresh) jnlForeign_ = true;
    jnlTail_ = !fresh;
    return false;
  }
  Chunk& k = chunks_[chunkN_++];
  k.offset = at;
  k.bytes = kChunkHeader + payload + kChunkTrailer;
  k.seq = cc::get32(h + 4);
  k.count = count;
  k.parser = cfg_.parserVersion;
  k.rules = mptg::kReadRules;
  k.epoch = cc::get32(h + 24);
  jnlEnd_ = at + k.bytes;
  jnlTail_ = false;
  jnlForeign_ = false;
  c.clear();
  return true;
}

// ---------------------------------------------------------------------------
// walk.jnl
// ---------------------------------------------------------------------------
namespace {

// The last key of each kind of a walk run (files, folders, doubts), for the
// checks of its order.
struct RunKeys {
  char path[3][cc::kMaxRelPath + 1];
  size_t len[3] = {};
  bool have[3] = {};
  bool settles = false;  // a Doubt entry to settle (an audio file's) came
};

// Checks a walk entry's head and tail in a stream, and that it comes after
// the last of its kind.
bool checkWalkEntry(cc::Stream& s, RunKeys& k, char* cur) {
  uint8_t h[kEntryHead];
  if (!s.read(h, kEntryHead)) return false;
  const uint8_t kind = h[0], flags = h[1];
  const uint16_t len = cc::get16(h + 2);
  if (len > cc::kMaxRelPath || !s.read(cur, len)) return false;
  cur[len] = 0;
  uint8_t t[kDoubtTail];
  int seq;  // 0 files, 1 folders, 2 doubts
  switch (static_cast<EntryKind>(kind)) {
    case EntryKind::File:
      if (!walkStatus(flags) || !s.read(t, kFileTail)) return false;
      seq = 0;
      break;
    case EntryKind::Gone:
      if (flags) return false;
      seq = 0;
      break;
    case EntryKind::Doubt:
      if ((flags & ~15u) || !s.read(t, kDoubtTail)) return false;
      if (flags & 1) k.settles = true;
      seq = 2;
      break;
    case EntryKind::Folder: {
      FolderFacts f;
      if (flags || !readFacts(s, &f)) return false;
      seq = 1;
      break;
    }
    case EntryKind::FolderGone:
      if (flags) return false;
      seq = 1;
      break;
    default:
      return false;
  }
  if (seq == 1 ? !validFolderPath(cur, len) : !cc::validRelPath(cur, len)) return false;
  if (k.have[seq]) {
    const int c = seq == 1 ? cc::compareFolderPaths(k.path[seq], k.len[seq], cur, len)
                           : cc::compareFilePaths(k.path[seq], k.len[seq], cur, len);
    if (c >= 0) return false;
  }
  std::memcpy(k.path[seq], cur, len + 1u);
  k.len[seq] = len;
  k.have[seq] = true;
  return true;
}

}  // namespace

void TagStore::scanWalk(Opened* o) {
  walkRuns_ = 0;
  walkSeq_ = 0;
  walkSkew_ = 0;
  walkUnsettled_ = false;
  walkId_ = Identity();
  File* f = fs_.open(walkPath_, Fs::Mode::Read);
  if (!f) return;
  const uint32_t size = f->size();
  uint8_t h[kWalkHeader];
  bool ok = size >= kWalkHeader && f->read(0, h, kWalkHeader) && cc::get32(h) == kWalkMagic &&
            cc::get16(h + 4) == kWalkVersion && cc::get32(h + 60) == cc::crc32(h, 60);
  if (ok && cc::get32(h + 8) != deviceCrc()) ok = false;
  void* mem = ok ? alloc_(cfg_.runBuffer + sizeof(RunKeys) + cc::kMaxRelPath + 1) : nullptr;
  if (mem) {
    uint8_t* buf = static_cast<uint8_t*>(mem);
    RunKeys* keys = reinterpret_cast<RunKeys*>(buf + cfg_.runBuffer);
    char* cur = reinterpret_cast<char*>(buf + cfg_.runBuffer + sizeof(RunKeys));
    uint32_t at = kWalkHeader;
    for (uint32_t run = 1; run <= 2; ++run) {
      const uint32_t start = at;
      uint32_t entries = 0;
      new (keys) RunKeys();
      bool complete = false;
      int32_t skew = 0;
      uint32_t endFlags = 0;
      for (;;) {
        uint8_t b[kBlockHeader];
        if (size - at < kBlockHeader + 4 || !f->read(at, b, kBlockHeader)) break;
        const uint32_t count = cc::get32(b + 8), payload = cc::get32(b + 12);
        if (cc::get32(b) != kBlockMagic || b[4] != run || payload > size - at - kBlockHeader - 4) break;
        cc::Stream s;
        s.begin(f, at, at + kBlockHeader + payload, buf, cfg_.runBuffer);
        if (!s.skipTo(at + kBlockHeader)) break;
        bool good = true;
        if (b[5] == kBlockEnd) {
          uint8_t e[kEndPayload];
          good = payload == kEndPayload && count == entries && s.read(e, kEndPayload);
          skew = static_cast<int32_t>(cc::get32(e));
          endFlags = cc::get32(e + 4);
        } else if (b[5] == kBlockEntries) {
          for (uint32_t k = 0; good && k < count; ++k) good = checkWalkEntry(s, *keys, cur);
          entries += count;
        } else {
          good = false;
        }
        uint8_t c[4];
        if (!good || s.position() != s.end() || !f->read(at + kBlockHeader + payload, c, 4) || cc::get32(c) != s.crc())
          break;
        at += kBlockHeader + payload + 4;
        if (b[5] == kBlockEnd) {
          complete = true;
          break;
        }
      }
      if (!complete) break;
      walkStart_[run - 1] = start;
      walkEnd_[run - 1] = at;
      walkSkew_ = skew;
      // Run 1 alone leaves its doubts unsettled; run 2's End says whether
      // one was left so.
      walkUnsettled_ = run == 1 ? keys->settles : (endFlags & kEndUnsettled) != 0;
      walkRuns_ = run;
    }
    free_(mem);
  }
  fs_.close(f);
  if (walkRuns_) {
    walkSeq_ = cc::get32(h + 12);
    const uint32_t flags = cc::get32(h + 16);
    walkId_.present = (flags & kWalkTransfer) != 0;
    walkId_.generation = cc::get32(h + 20);
    walkId_.cardId = cc::get64(h + 24);
    walkId_.commitId = cc::get64(h + 32);
    walkId_.tagsCrc = cc::get32(h + 40);
  }
  o->walk = walkRuns_ > 0;
  o->walkStale = walkRuns_ == 0;
}

bool TagStore::beginWalk(const Identity& id, uint8_t* buf, uint32_t bytes, WalkWriter* w) {
  if (!w || walking_ || walkRuns_ > 0 || !buf || bytes < 1024) return false;
  *w = WalkWriter();
  w->store_ = this;
  w->buf_ = buf;
  w->cap_ = bytes;
  std::memset(w->header_, 0, sizeof(w->header_));
  cc::put32(w->header_, kWalkMagic);
  cc::put16(w->header_ + 4, kWalkVersion);
  cc::put32(w->header_ + 8, deviceCrc());
  cc::put32(w->header_ + 12, journalSeq());
  cc::put32(w->header_ + 16, id.present ? kWalkTransfer : 0u);
  cc::put32(w->header_ + 20, id.generation);
  cc::put64(w->header_ + 24, id.cardId);
  cc::put64(w->header_ + 32, id.commitId);
  cc::put32(w->header_ + 40, id.tagsCrc);
  cc::put32(w->header_ + 60, cc::crc32(w->header_, 60));
  w->id_ = id;
  walking_ = true;
  return true;
}

bool WalkWriter::ensureFile() {
  if (open_) return !failed_;
  Fs& fs = store_->fs_;
  file_ = fs.open(store_->walkPath_, Fs::Mode::Create);
  if (!file_) {
    failed_ = true;
    return false;
  }
  open_ = true;  // finish() closes it (abort() removes it) whatever comes next
  if (!file_->write(0, header_, kWalkHeader)) {
    failed_ = true;
    return false;
  }
  at_ = kWalkHeader;
  runStart_[0] = at_;
  return true;
}

bool WalkWriter::writeBlock(uint8_t type, const uint8_t* payload, uint32_t bytes, uint32_t count) {
  uint8_t h[kBlockHeader] = {};
  cc::put32(h, kBlockMagic);
  h[4] = static_cast<uint8_t>(run_);
  h[5] = type;
  cc::put32(h + 8, count);
  cc::put32(h + 12, bytes);
  uint32_t crc = cc::crc32(h, kBlockHeader);
  crc = cc::crc32(payload, bytes, crc);
  uint8_t c[4];
  cc::put32(c, crc);
  if (!file_->write(at_, h, kBlockHeader) || (bytes && !file_->write(at_ + kBlockHeader, payload, bytes)) ||
      !file_->write(at_ + kBlockHeader + bytes, c, 4) || !file_->sync()) {
    failed_ = true;
    return false;
  }
  at_ += kBlockHeader + bytes + 4;
  return true;
}

bool WalkWriter::writeEnd(uint32_t run, int32_t skew, uint32_t count, uint32_t flags) {
  const uint32_t r = run_;
  run_ = run;
  uint8_t e[kEndPayload] = {};
  cc::put32(e, static_cast<uint32_t>(skew));
  cc::put32(e + 4, flags);
  const bool ok = writeBlock(kBlockEnd, e, kEndPayload, count);
  run_ = r;
  if (!ok) return false;
  runEnd_[run - 1] = at_;
  if (run == 1) runStart_[1] = at_;
  return true;
}

bool WalkWriter::flush() {
  if (blockCount_ == 0) return true;
  if (!ensureFile()) return false;
  if (deferred_) {
    // Run 1 found nothing new and run 2 has something: run 1's End first.
    if (!writeEnd(1, skew1_, 0)) return false;
    deferred_ = false;
  }
  if (!writeBlock(kBlockEntries, buf_, fill_, blockCount_)) return false;
  fill_ = 0;
  blockCount_ = 0;
  return true;
}

bool WalkWriter::sameAsDevice(int32_t skew) const {
  const DeviceInfo& d = store_->dev_;
  return d.present && d.header.walked && d.header.walk == id_ && d.header.skew == skew;
}

bool WalkWriter::put(EntryKind kind, uint8_t flags, const char* rel, size_t len, const uint8_t* tail,
                     uint32_t tailLen) {
  if (failed_ || !store_ || run_ > 2 || !rel) return false;
  const bool isFolder = kind == EntryKind::Folder || kind == EntryKind::FolderGone;
  if (isFolder ? !validFolderPath(rel, len) : !cc::validRelPath(rel, len)) return false;
  Key& k = isFolder ? folders_ : (kind == EntryKind::Doubt ? doubts_ : files_);
  if (k.have && (isFolder ? cc::compareFolderPaths(k.path, k.len, rel, len)
                          : cc::compareFilePaths(k.path, k.len, rel, len)) >= 0) {
    failed_ = true;  // never write a run out of order: the walk is dropped
    return false;
  }
  const uint32_t bytes = kEntryHead + static_cast<uint32_t>(len) + tailLen;
  if (bytes > cap_) {
    failed_ = true;
    return false;
  }
  if (fill_ + bytes > cap_ && !flush()) return false;
  uint8_t* p = buf_ + fill_;
  p[0] = static_cast<uint8_t>(kind);
  p[1] = flags;
  cc::put16(p + 2, static_cast<uint16_t>(len));
  std::memcpy(p + kEntryHead, rel, len);
  if (tailLen) std::memcpy(p + kEntryHead + len, tail, tailLen);
  fill_ += bytes;
  ++blockCount_;
  ++runCount_;
  std::memcpy(k.path, rel, len);
  k.path[len] = 0;
  k.len = len;
  k.have = true;
  return true;
}

bool WalkWriter::file(const char* rel, size_t len, uint32_t size, uint32_t fatTime, uint64_t qfp, Status status,
                      bool confirmed) {
  uint8_t t[kFileTail];
  cc::put32(t, size);
  cc::put32(t + 4, fatTime);
  cc::put64(t + 8, qfp);
  return put(EntryKind::File, encodeRow(Row{status, confirmed}), rel, len, t, kFileTail);
}

bool WalkWriter::gone(const char* rel, size_t len) { return put(EntryKind::Gone, 0, rel, len, nullptr, 0); }

bool WalkWriter::doubt(const char* rel, size_t len, const Doubt& d) {
  uint8_t t[kDoubtTail];
  cc::put32(t, d.size);
  cc::put32(t + 4, d.fatTime);
  cc::put64(t + 8, static_cast<uint64_t>(d.delta));
  cc::put64(t + 16, d.transferQfp);
  cc::put64(t + 24, d.deviceQfp);
  const uint8_t flags = static_cast<uint8_t>((d.settle ? 1u : 0u) | (d.hasDelta ? 2u : 0u) |
                                             ((static_cast<uint8_t>(d.fallback) & 3u) << 2));
  if (!put(EntryKind::Doubt, flags, rel, len, t, kDoubtTail)) return false;
  if (d.settle && run_ == 1) settles_ = true;
  return true;
}

bool WalkWriter::folder(const char* rel, size_t len, const FolderFacts& facts) {
  if (facts.imageLength && !cc::validName(facts.image, facts.imageLength)) return false;
  uint8_t t[kFactsFixed + 256];
  const uint32_t n = encodeFacts(facts, t);
  return put(EntryKind::Folder, 0, rel, len, t, n);
}

bool WalkWriter::folderGone(const char* rel, size_t len) {
  return put(EntryKind::FolderGone, 0, rel, len, nullptr, 0);
}

bool WalkWriter::endRun(int32_t skew, bool unsettled) {
  if (failed_ || !store_ || run_ > 2) return false;
  const bool nothing = runCount_ == 0 && !open_ && !unsettled && sameAsDevice(skew) && (run_ == 1 || deferred_);
  if (nothing) {
    // Nothing new: nothing reaches the card (unless run 2 finds something).
    if (run_ == 1) {
      deferred_ = true;
      skew1_ = skew;
    }
  } else {
    if (!flush() || !ensureFile()) return false;
    if (deferred_) {
      // Run 1 found nothing, run 2 ends with a new skew: run 1's End first.
      if (!writeEnd(1, skew1_, 0)) return false;
      deferred_ = false;
    }
    if (!writeEnd(run_, skew, runCount_, unsettled ? kEndUnsettled : 0)) return false;
    // Merged as it is now, run 1 leaves its doubts unsettled.
    store_->walkDone(run_, runStart_, runEnd_, skew, id_, cc::get32(header_ + 12), run_ == 1 ? settles_ : unsettled);
  }
  ++run_;
  runCount_ = 0;
  files_.have = folders_.have = doubts_.have = false;
  return true;
}

bool WalkWriter::finish() {
  bool ok = !failed_;
  if (open_ && file_) ok = store_->fs_.close(file_) && ok;
  file_ = nullptr;
  open_ = false;
  if (store_) store_->walking_ = false;
  run_ = 3;
  return ok;
}

void WalkWriter::abort() {
  if (!store_) return;
  const bool written = open_;
  finish();
  if (written && store_->fs_.exists(store_->walkPath_)) store_->fs_.remove(store_->walkPath_);
  store_->walkRuns_ = 0;
}

void TagStore::walkDone(uint32_t runs, const uint32_t* start, const uint32_t* end, int32_t skew, const Identity& id,
                        uint32_t seq, bool unsettled) {
  walkRuns_ = runs;
  walkUnsettled_ = unsettled;
  for (uint32_t r = 0; r < runs; ++r) {
    walkStart_[r] = start[r];
    walkEnd_[r] = end[r];
  }
  walkSkew_ = skew;
  walkId_ = id;
  walkSeq_ = seq;
}

bool TagStore::readWalk(uint32_t run, uint8_t* buf, uint32_t bytes, WalkReader* out) {
  if (!out || run < 1 || run > walkRuns_ || !buf || bytes < 64) return false;
  out->close();
  File* f = fs_.open(walkPath_, Fs::Mode::Read);
  if (!f) return false;
  out->fs_ = &fs_;
  out->f_ = f;
  out->s_.begin(f, walkStart_[run - 1], walkEnd_[run - 1], buf, bytes);
  out->left_ = 0;
  out->inBlock_ = false;
  out->done_ = out->failed_ = false;
  return true;
}

void WalkReader::close() {
  if (f_ && fs_) fs_->close(f_);
  f_ = nullptr;
}

WalkReader::~WalkReader() { close(); }

bool WalkReader::next(WalkEntry* e) {
  if (done_ || failed_ || !f_) return false;
  auto bad = [this]() {
    failed_ = true;
    return false;
  };
  for (;;) {
    if (inBlock_ && left_ == 0) {
      uint8_t crc[4];
      if (!s_.read(crc, 4)) return bad();
      inBlock_ = false;
    }
    if (!inBlock_) {
      uint8_t h[kBlockHeader];
      if (!s_.read(h, kBlockHeader) || cc::get32(h) != kBlockMagic) return bad();
      if (h[5] == kBlockEnd) {
        done_ = true;
        return false;
      }
      left_ = cc::get32(h + 8);
      inBlock_ = true;
      continue;
    }
    uint8_t h[kEntryHead];
    if (!s_.read(h, kEntryHead)) return bad();
    --left_;
    e->kind = static_cast<EntryKind>(h[0]);
    e->pathLen = cc::get16(h + 2);
    if (e->pathLen > cc::kMaxRelPath || !s_.read(e->path, static_cast<uint32_t>(e->pathLen))) return bad();
    e->path[e->pathLen] = 0;
    e->row = Row();
    e->size = e->fatTime = 0;
    e->qfp = 0;
    e->doubt = Doubt();
    e->facts = FolderFacts();
    uint8_t t[kDoubtTail];
    switch (e->kind) {
      case EntryKind::File:
        if (!s_.read(t, kFileTail)) return bad();
        e->row = decodeRow(h[1]);
        e->size = cc::get32(t);
        e->fatTime = cc::get32(t + 4);
        e->qfp = cc::get64(t + 8);
        break;
      case EntryKind::Doubt:
        if (!s_.read(t, kDoubtTail)) return bad();
        e->doubt.settle = (h[1] & 1) != 0;
        e->doubt.hasDelta = (h[1] & 2) != 0;
        e->doubt.fallback = static_cast<Status>((h[1] >> 2) & 3u);
        e->doubt.size = cc::get32(t);
        e->doubt.fatTime = cc::get32(t + 4);
        e->doubt.delta = static_cast<int64_t>(cc::get64(t + 8));
        e->doubt.transferQfp = cc::get64(t + 16);
        e->doubt.deviceQfp = cc::get64(t + 24);
        break;
      case EntryKind::Gone:
      case EntryKind::FolderGone:
        break;
      case EntryKind::Folder:
        if (!readFacts(s_, &e->facts)) return bad();
        break;
      default:
        return bad();
    }
    return true;
  }
}

// ---------------------------------------------------------------------------
// The compaction.
// ---------------------------------------------------------------------------
namespace {

// The new D's sections, in their order.
enum OutSec : uint32_t { kOFold = 0, kORecs, kOStrs, kOHidx, kODsta, kODfld, kODhdr, kOCount };

struct Level {
  uint32_t index;
  uint16_t end;
  bool own;
};

}  // namespace

struct CompactWork {
  Merge m;
  // The output.
  bool writing = false;
  uint32_t records = 0, folders = 0, ownRecords = 0, ownFolders = 0;
  uint32_t strsFolder = 0, strsRecords = 0, dfldBytes = 0;
  uint32_t split = 0;
  Cursor fold, recs, strsF, strsR, dsta, dfld, hidx;
  Level stack[mptg::Walker::kMaxDepth + 2];
  uint32_t depth = 0;
  char path[cc::kMaxRelPath + 1];
  Distinct albums, artists;
  ValueHasher hasher;
  // The folders' facts.
  bool binFacts = false;
  FolderCursor binFolders;
  WalkFolders walk[2];
  uint32_t walkRuns = 0;
  FolderFacts facts, tmpFacts;
  uint8_t factsBuf[kFactsFixed + 256];
  // Buffers.
  uint8_t* binFolderBuf = nullptr;
  uint8_t* walkBuf[2] = {};
  uint8_t* outBuf[7] = {};
  uint8_t* arena = nullptr;
  size_t arenaBytes = 0;
};

namespace {

// The facts for the output's folder `rel`: D's, then walk run 1's, run 2's.
const FolderFacts& factsFor(CompactWork& w, const char* rel, size_t len) {
  w.facts = FolderFacts();
  if (w.binFacts && w.binFolders.find(rel, len, &w.tmpFacts)) w.facts = w.tmpFacts;
  for (uint32_t r = 0; r < w.walkRuns; ++r)
    if (w.walk[r].find(rel, len, &w.tmpFacts)) w.facts = w.tmpFacts;
  return w.facts;
}

bool emitFolder(CompactWork& w, uint32_t parent, const char* name, size_t nameLen) {
  uint8_t row[mptg::kFoldStride];
  mptg::Folder f;
  f.parent = parent;
  f.name = nameLen ? w.strsFolder : 0;
  f.flags = 0;
  f.firstRecord = w.records;
  mptg::encodeFolder(f, row);
  ++w.folders;
  if (!w.fold.put(row, mptg::kFoldStride)) return false;
  if (nameLen) {
    if (!w.strsF.put(name, static_cast<uint32_t>(nameLen)) || !w.strsF.putByte(0)) return false;
    w.strsFolder += static_cast<uint32_t>(nameLen) + 1;
  }
  const uint32_t n = encodeFacts(factsFor(w, w.path, w.stack[w.depth - 1].end), w.factsBuf);
  w.dfldBytes += n;
  return w.dfld.put(w.factsBuf, n);
}

// Enters the folders of `rel` (a record's path) the output hasn't.
bool enterFolders(CompactWork& w, const char* rel, size_t len) {
  const size_t folderLen = folderLength(rel, len);
  while (w.depth > 1 && !ancestorOrSelf(w.path, w.stack[w.depth - 1].end, rel, folderLen)) --w.depth;
  size_t at = w.stack[w.depth - 1].end;
  while (at < folderLen) {
    const size_t start = at ? at + 1 : 0;
    size_t end = start;
    while (end < folderLen && rel[end] != '/') ++end;
    if (w.depth > mptg::Walker::kMaxDepth) return false;
    std::memcpy(w.path + start, rel + start, end - start);
    if (start) w.path[start - 1] = '/';
    w.path[end] = 0;
    const uint32_t index = w.folders;
    w.stack[w.depth++] = Level{index, static_cast<uint16_t>(end), false};
    if (!emitFolder(w, w.stack[w.depth - 2].index, rel + start, end - start)) return false;
    at = end;
  }
  return true;
}

bool emitRecord(CompactWork& w) {
  Merge& m = w.m;
  if (!enterFolders(w, m.path, m.len)) return false;
  const size_t folderLen = folderLength(m.path, m.len);
  const size_t nameAt = folderLen ? folderLen + 1 : 0;
  const char* name = m.path + nameAt;
  const uint32_t nameLen = static_cast<uint32_t>(m.len - nameAt);
  const uint32_t runLen = m.runLength();
  mptg::Record r = m.rec;
  r.folder = w.stack[w.depth - 1].index;
  r.name = w.split + w.strsRecords;
  r.strings = runLen ? r.name + nameLen + 1 : 0;
  uint8_t row[mptg::kRecsStride];
  mptg::encodeRecord(r, row);
  if (!w.recs.put(row, mptg::kRecsStride)) return false;
  if (!w.strsR.put(name, nameLen) || !w.strsR.putByte(0)) return false;
  if (!m.copyRun(&w.strsR, w.writing ? nullptr : &w.hasher)) return false;
  w.strsRecords += nameLen + 1 + runLen;
  if (!w.dsta.putByte(encodeRow(m.row))) return false;
  if (!w.writing) {
    uint8_t e[mptg::kHidxStride];
    cc::put64(e, cc::pathHash(m.path, m.len));
    cc::put32(e + 8, w.records);
    if (!w.hidx.put(e, mptg::kHidxStride)) return false;
  }
  if (m.row.status != Status::Software) {
    ++w.ownRecords;
    for (uint32_t k = w.depth; k-- > 0 && !w.stack[k].own;) {
      w.stack[k].own = true;
      ++w.ownFolders;
    }
  }
  ++w.records;
  return true;
}

}  // namespace

size_t TagStore::workBytes() const {
  // The fixed part (the output, the folder cursors and their buffers),
  // then the arena, as compact() takes them.
  return sizeof(CompactWork) + 5 * cfg_.runBuffer + 7 * cfg_.writeBuffer + 16 * 8 + arenaBytes(cfg_, cfg_.maxChunks);
}

bool TagStore::openView(View* v) {
  if (!v) return false;
  v->~View();
  new (v) View();
  const size_t arena = arenaBytes(cfg_, chunkN_);
  const size_t bytes = sizeof(Merge) + cc::kRunMax + arena + 24;
  void* mem = alloc_(bytes);
  if (!mem) return false;
  Bump b{static_cast<uint8_t*>(mem), bytes};
  Merge* m = new (b.take(sizeof(Merge))) Merge();
  v->runBuf_ = static_cast<uint8_t*>(b.take(cc::kRunMax));
  v->m_ = m;
  v->store_ = this;
  v->mem_ = mem;
  if (!beginMerge(*m, static_cast<uint8_t*>(b.take(arena)), arena, dev_.present, dev_.header.epoch)) {
    m->failed = true;
    return false;
  }
  return true;
}

TagStore::View::~View() {
  if (m_) {
    m_->closeFiles();
    if (m_->dev) m_->dev->~DeviceReader();
    m_->~Merge();
  }
  if (mem_ && store_) store_->release(mem_);
  m_ = nullptr;
  mem_ = nullptr;
}

bool TagStore::View::next() { return m_ && m_->next(); }
const char* TagStore::View::path() const { return m_ ? m_->path : ""; }
size_t TagStore::View::pathLength() const { return m_ ? m_->len : 0; }
const Row& TagStore::View::row() const { return m_->row; }
const mptg::Record& TagStore::View::record() const { return m_->rec; }
bool TagStore::View::failed() const { return !m_ || m_->failed; }

bool TagStore::View::run(cc::RunFields* out) {
  out->clear();
  if (!m_ || m_->runFrom < 0) return true;
  Input& i = m_->in[m_->runFrom];
  if (i.type == kInBin) {
    *out = *m_->dev->walker().run();
    return true;
  }
  if (!runBuf_ || !i.runPending || i.runLen > cc::kRunMax || !i.s.read(runBuf_, i.runLen)) return false;
  i.runPending = false;
  m_->runFrom = -1;
  return cc::parseRun(runBuf_, i.runLen, out);
}

// Sets up the merge's inputs in the arena: D (when `useBin`), the chunks
// older than the walk, the walk's runs, the chunks after it.
bool TagStore::beginMerge(Merge& m, uint8_t* arena, size_t arenaBytes, bool useBin, uint32_t epoch) {
  m.fs = &fs_;
  Bump b{arena, arenaBytes};
  const uint32_t maxIn = 1 + 2 + chunkN_;
  m.in = static_cast<Input*>(b.take(sizeof(Input) * maxIn));
  if (!m.in) return false;
  m.n = 0;
  auto add = [&](uint8_t type) -> Input* {
    Input* i = new (&m.in[m.n++]) Input();
    i->type = type;
    return i;
  };
  if (useBin) {
    void* dev = b.take(sizeof(DeviceReader));
    void* run = b.take(sizeof(cc::RunFields));
    uint8_t* scratch = static_cast<uint8_t*>(b.take(cfg_.deviceBuffer));
    if (!dev || !run || !scratch) return false;
    m.dev = new (dev) DeviceReader();
    m.binRun = new (run) cc::RunFields();
    m.binF = fs_.open(binPath_, Fs::Mode::Read);
    const Why w = m.binF ? m.dev->begin(*m.binF, scratch, cfg_.deviceBuffer, m.binRun, false) : Why::Io;
    if (w != Why::Ok) {
      m.binFailed = true;  // D is left out
      return false;
    }
    const DeviceInfo& di = m.dev->info();
    m.binConvert = di.tags.parserVersion != cfg_.parserVersion || di.tags.readRules != mptg::kReadRules ||
                   di.header.epoch != epoch;
    m.useBin = true;
    Input* i = add(kInBin);
    i->advance = true;
  }
  if (chunkN_) {
    m.jnlF = fs_.open(jnlPath_, Fs::Mode::Read);
    if (!m.jnlF) return false;
  }
  if (walkRuns_) {
    m.walkF = fs_.open(walkPath_, Fs::Mode::Read);
    if (!m.walkF) return false;
  }
  auto addChunk = [&](const Chunk& k) -> bool {
    uint8_t* buf = static_cast<uint8_t*>(b.take(cfg_.runBuffer));
    if (!buf) return false;
    Input* i = add(kInChunk);
    const uint32_t payload = k.bytes - kChunkHeader - kChunkTrailer;
    i->s.begin(m.jnlF, k.offset + kChunkHeader, k.offset + kChunkHeader + payload, buf, cfg_.runBuffer);
    i->left = k.count;
    i->convert = k.parser != cfg_.parserVersion || k.rules != mptg::kReadRules || k.epoch != epoch;
    i->advance = true;
    return true;
  };
  uint32_t c = 0;
  if (walkRuns_)
    for (; c < chunkN_ && chunks_[c].seq <= walkSeq_; ++c)
      if (!addChunk(chunks_[c])) return false;
  for (uint32_t r = 0; r < walkRuns_; ++r) {
    uint8_t* buf = static_cast<uint8_t*>(b.take(cfg_.runBuffer));
    if (!buf) return false;
    Input* i = add(kInWalk);
    i->s.begin(m.walkF, walkStart_[r], walkEnd_[r], buf, cfg_.runBuffer);
    i->advance = true;
  }
  for (; c < chunkN_; ++c)
    if (!addChunk(chunks_[c])) return false;
  return true;
}

TagStore::Compacted TagStore::compact(bool rescan) {
  Compacted res;
  auto fail = [&](const char* e) {
    res.ok = false;
    res.error = e;
    return res;
  };
  if (walking_) return fail("a walk is being written");
  const uint32_t epoch = (dev_.present ? dev_.header.epoch : 0) + (rescan ? 1u : 0u);

  // The work memory: the fixed part, then the arena.
  const size_t arena = arenaBytes(cfg_, chunkN_);
  const size_t bytes = sizeof(CompactWork) + 5 * cfg_.runBuffer + 7 * cfg_.writeBuffer + 16 * 8 + arena;
  void* mem = alloc_(bytes);
  if (!mem) return fail("no memory");
  res.workBytes = bytes;
  Bump b{static_cast<uint8_t*>(mem), bytes};
  // Zeroed, then default-initialized in place: what `new (p) CompactWork()`
  // means, without the copy of it xtensa's GCC built on the stack for that
  // value-initialization (a 13 KB frame: compact() runs on the card
  // worker's 6 KB stack; N12's review, -fstack-usage).
  void* wm = b.take(sizeof(CompactWork));
  std::memset(wm, 0, sizeof(CompactWork));
  CompactWork* w = new (wm) CompactWork;
  w->binFolderBuf = static_cast<uint8_t*>(b.take(3 * cfg_.runBuffer));
  for (int r = 0; r < 2; ++r) w->walkBuf[r] = static_cast<uint8_t*>(b.take(cfg_.runBuffer));
  for (int k = 0; k < 7; ++k) w->outBuf[k] = static_cast<uint8_t*>(b.take(cfg_.writeBuffer));
  w->arena = static_cast<uint8_t*>(b.take(arena));
  w->arenaBytes = arena;

  File* hidxF = nullptr;
  File* binF = nullptr;
  File* walkF = nullptr;
  File* out = nullptr;
  auto cleanup = [&]() {
    w->m.closeFiles();
    if (w->m.dev) w->m.dev->~DeviceReader();
    w->m.dev = nullptr;
    if (hidxF) fs_.close(hidxF);
    if (binF) fs_.close(binF);
    if (walkF) fs_.close(walkF);
    if (out) fs_.close(out);
    hidxF = binF = walkF = out = nullptr;
  };
  bool tmpMade = false;
  auto finish = [&](const char* e) {
    cleanup();
    // A tags.tmp of ours is new (nothing shares its chain): it can go.
    if (tmpMade) fs_.remove(tmpPath_);
    if (fs_.exists(hidxPath_)) fs_.remove(hidxPath_);
    w->~CompactWork();
    free_(mem);
    if (e) {
      res.ok = false;
      res.error = e;
    }
    return res;
  };

  // One pass of the merge into the output (counting, or writing).
  bool useBin = dev_.present;
  bool binFacts = useBin;
  auto pass = [&](bool writing) -> int {  // 1 done, 0 D failed, -1 failed
    w->m.closeFiles();
    if (w->m.dev) w->m.dev->~DeviceReader();
    Merge& m = w->m;
    m = Merge();
    w->writing = writing;
    w->records = w->folders = w->ownRecords = w->ownFolders = 0;
    w->strsFolder = w->strsRecords = w->dfldBytes = 0;
    w->albums.clear();
    w->artists.clear();
    w->hasher.albums = &w->albums;
    w->hasher.artists = &w->artists;
    if (!beginMerge(m, w->arena, w->arenaBytes, useBin, epoch)) return m.binFailed ? 0 : -1;
    // The folders' facts.
    w->binFacts = false;
    if (binFacts) {
      if (binF) fs_.close(binF);
      binF = fs_.open(binPath_, Fs::Mode::Read);
      w->binFacts = binF && w->binFolders.begin(*binF, w->binFolderBuf, 3 * cfg_.runBuffer);
    }
    if (walkF) fs_.close(walkF);
    walkF = nullptr;
    w->walkRuns = walkRuns_;
    if (walkRuns_) {
      walkF = fs_.open(walkPath_, Fs::Mode::Read);
      if (!walkF) return -1;
      for (uint32_t r = 0; r < walkRuns_; ++r)
        w->walk[r].begin(walkF, walkStart_[r], walkEnd_[r], w->walkBuf[r], cfg_.runBuffer);
    }
    // The root, then the records.
    w->depth = 1;
    w->stack[0] = Level{0, 0, false};
    w->path[0] = 0;
    // STRS's leading NUL and the producer.
    const char* producer = cfg_.producer ? cfg_.producer : "";
    const uint32_t plen = static_cast<uint32_t>(std::strlen(producer));
    if (!w->strsF.putByte(0)) return -1;
    w->strsFolder = 1;
    if (plen) {
      if (!w->strsF.put(producer, plen) || !w->strsF.putByte(0)) return -1;
      w->strsFolder += plen + 1;
    }
    if (!emitFolder(*w, mptg::kNoParent, nullptr, 0)) return -1;
    while (m.next())
      if (!emitRecord(*w)) return -1;
    if (m.failed) return m.binFailed ? 0 : -1;
    if (m.useBin && !m.binEnded) return 0;
    if (!writing && w->binFacts && !w->binFolders.finish()) return 2;  // DFLD bad: again without D's facts
    return 1;
  };

  // Pass 1: the counts, and HIDX's pairs into hidx.tmp.
  Cursor* outs[7] = {&w->fold, &w->recs, &w->strsF, &w->strsR, &w->dsta, &w->dfld, &w->hidx};
  int r;
  for (;;) {
    for (Cursor* c : outs) c->begin(nullptr, 0, nullptr, 0);
    if (hidxF) fs_.close(hidxF);
    hidxF = fs_.open(hidxPath_, Fs::Mode::Create);
    if (!hidxF) return finish("hidx.tmp");
    w->hidx.begin(hidxF, 0, w->outBuf[6], cfg_.writeBuffer);
    r = pass(false);
    if (r == 0 && useBin) {
      // D failed its checks: without it (3.4.1); the walk and the scan rebuild it.
      res.deviceWhy = w->m.dev ? w->m.dev->why() : Why::Io;
      if (res.deviceWhy == Why::Ok) res.deviceWhy = Why::Io;
      useBin = binFacts = false;
      continue;
    }
    if (r == 2) {
      binFacts = false;
      continue;
    }
    break;
  }
  if (r != 1) return finish("read");
  if (!w->hidx.flush()) return finish("hidx.tmp");
  res.rescanned = w->m.converted;
  const uint32_t records = w->records, folders = w->folders, ownRecords = w->ownRecords, ownFolders = w->ownFolders;
  const uint32_t strsFolder = w->strsFolder, strsRecords = w->strsRecords, dfldBytes = w->dfldBytes;
  const uint32_t albumValues = w->albums.count(), artistValues = w->artists.count();

  // The layout (2.4.2): FOLD, RECS, STRS, HIDX (with records), DSTA, DFLD, DHDR.
  struct Sec {
    uint32_t type, flags, count, stride, bytes, offset, crc;
    bool on;
  };
  Sec s[kOCount] = {
      {mptg::kFold, cc::kSectionRequired, folders, mptg::kFoldStride, folders * mptg::kFoldStride, 0, 0, true},
      {mptg::kRecs, cc::kSectionRequired, records, mptg::kRecsStride, records * mptg::kRecsStride, 0, 0, true},
      {cc::kStrs, cc::kSectionRequired, 0, 0, strsFolder + strsRecords, 0, 0, true},
      {mptg::kHidx, 0, records, mptg::kHidxStride, records * mptg::kHidxStride, 0, 0, records > 0},
      {kDsta, 0, 0, 0, kDstaHeader + records, 0, 0, true},
      {kDfld, 0, 0, 0, kDfldHeader + dfldBytes, 0, 0, true},
      {kDhdr, 0, 0, 0, kDhdrBytes, 0, 0, true},
  };
  uint32_t nSec = 0;
  for (const Sec& x : s) nSec += x.on ? 1 : 0;
  uint64_t at = mptg::kHeaderBytes + cc::kDirEntryBytes * nSec;
  for (Sec& x : s) {
    if (!x.on) continue;
    at = align8(static_cast<uint32_t>(at));
    x.offset = static_cast<uint32_t>(at);
    at += x.bytes;
    if (at > 0xFFFFFFF0ull) return finish("too big");
  }
  const uint32_t fileBytes = static_cast<uint32_t>(at);

  // Pass 2: the sections at their places.
  if (!prepareTmp(fs_, names_)) return finish("tags.tmp");
  out = fs_.open(tmpPath_, Fs::Mode::Create);
  if (!out) return finish("tags.tmp");
  tmpMade = true;
  w->fold.begin(out, s[kOFold].offset, w->outBuf[0], cfg_.writeBuffer);
  w->recs.begin(out, s[kORecs].offset, w->outBuf[1], cfg_.writeBuffer);
  w->strsF.begin(out, s[kOStrs].offset, w->outBuf[2], cfg_.writeBuffer);
  w->strsR.begin(out, s[kOStrs].offset + strsFolder, w->outBuf[3], cfg_.writeBuffer);
  w->dsta.begin(out, s[kODsta].offset, w->outBuf[4], cfg_.writeBuffer);
  w->dfld.begin(out, s[kODfld].offset, w->outBuf[5], cfg_.writeBuffer);
  w->hidx.begin(nullptr, 0, nullptr, 0);
  w->split = strsFolder;
  {
    uint8_t h[kDstaHeader] = {};
    cc::put32(h, records);
    cc::put32(h + 4, ownRecords);
    cc::put32(h + 8, ownFolders);
    if (!w->dsta.put(h, kDstaHeader)) return finish("write");
    uint8_t f[kDfldHeader] = {};
    cc::put32(f, folders);
    if (!w->dfld.put(f, kDfldHeader)) return finish("write");
  }
  r = pass(true);
  if (r != 1) return finish(r == 0 ? "D changed between the passes" : "read");
  if (w->records != records || w->folders != folders || w->strsFolder != strsFolder || w->strsRecords != strsRecords ||
      w->dfldBytes != dfldBytes)
    return finish("the passes disagree");
  for (int k = 0; k < 6; ++k)
    if (!outs[k]->flush()) return finish("write");
  s[kOFold].crc = w->fold.crc;
  s[kORecs].crc = w->recs.crc;
  s[kOStrs].crc = cc::crc32Combine(w->strsF.crc, w->strsR.crc, w->strsR.total);
  s[kODsta].crc = w->dsta.crc;
  s[kODfld].crc = w->dfld.crc;
  w->m.closeFiles();
  if (w->m.dev) w->m.dev->~DeviceReader();
  w->m.dev = nullptr;
  if (binF) fs_.close(binF);
  binF = nullptr;
  if (walkF) fs_.close(walkF);
  walkF = nullptr;

  // HIDX: sorted in passes over hidx.tmp, in the arena the merge gave back.
  if (records) {
    struct Pair {
      uint64_t h;
      uint32_t r;
    };
    auto less = [](const Pair& a, const Pair& b) { return a.h != b.h ? a.h < b.h : a.r < b.r; };
    uint8_t* sbuf = w->arena;
    Pair* heap = reinterpret_cast<Pair*>(w->arena + cfg_.runBuffer);
    const uint32_t cap = static_cast<uint32_t>((w->arenaBytes - cfg_.runBuffer) / sizeof(Pair));
    Cursor& hc = w->hidx;
    hc.begin(out, s[kOHidx].offset, w->outBuf[6], cfg_.writeBuffer);
    uint32_t emitted = 0;
    Pair last{0, 0};
    while (emitted < records) {
      cc::Stream st;
      st.begin(hidxF, 0, records * mptg::kHidxStride, sbuf, cfg_.runBuffer);
      uint32_t n = 0;
      for (uint32_t i = 0; i < records; ++i) {
        uint8_t e[mptg::kHidxStride];
        if (!st.read(e, mptg::kHidxStride)) return finish("hidx.tmp");
        const Pair p{cc::get64(e), cc::get32(e + 8)};
        if (emitted && !less(last, p)) continue;
        if (n < cap) {
          heap[n++] = p;
          std::push_heap(heap, heap + n, less);
        } else if (less(p, heap[0])) {
          std::pop_heap(heap, heap + n, less);
          heap[n - 1] = p;
          std::push_heap(heap, heap + n, less);
        }
      }
      if (n == 0) return finish("hidx.tmp");
      std::sort_heap(heap, heap + n, less);
      for (uint32_t i = 0; i < n; ++i) {
        uint8_t e[mptg::kHidxStride];
        cc::put64(e, heap[i].h);
        cc::put32(e + 8, heap[i].r);
        if (!hc.put(e, mptg::kHidxStride)) return finish("write");
      }
      last = heap[n - 1];
      emitted += n;
      ++res.hidxPasses;
    }
    if (!hc.flush()) return finish("write");
    s[kOHidx].crc = hc.crc;
  }
  fs_.close(hidxF);
  hidxF = nullptr;

  // DHDR: the walk merged, else D's (none when D was left out).
  DeviceHeader dh;
  if (useBin) dh = dev_.header;
  dh.epoch = epoch;
  if (walkRuns_ == 2 && !walkUnsettled_ && (useBin || !dev_.present)) {
    // A whole walk (its doubts settled) completes D's listing: its commit and
    // its skew are D's. Against a D left out, the walk's changes aren't a
    // listing (an unchanged folder said nothing): the next walk compares the
    // whole card.
    dh.walked = true;
    dh.walk = walkId_;
    dh.skew = walkSkew_;
  } else if (walkUnsettled_) {
    // Doubts left unsettled: run 1 alone (cut while they were settled), or
    // a qfp read that failed. The rows merge, D unwalked, so the next boot's
    // walk is a first one, against T, and settles them. (Kept at its commit,
    // a walk at the same commit would find those files' rows at their sizes
    // and times, and their folders' digests D's, and never ask.) Run 1 alone
    // with no doubt merges its rows and leaves DHDR as it was.
    dh.walked = false;
  }
  {
    uint8_t d[kDhdrBytes];
    encodeHeader(dh, d);
    s[kODhdr].crc = cc::crc32(d, kDhdrBytes);
    if (!out->write(s[kODhdr].offset, d, kDhdrBytes)) return finish("write");
  }
  // The gaps between sections: zeros.
  {
    static const uint8_t kZeros[8] = {};
    uint32_t end = mptg::kHeaderBytes + cc::kDirEntryBytes * nSec;
    for (const Sec& x : s) {
      if (!x.on) continue;
      if (x.offset > end && !out->write(end, kZeros, x.offset - end)) return finish("write");
      end = x.offset + x.bytes;
    }
  }
  // The directory and the header, last; a headerCrc of 0 would read as "no
  // D" in the journals' base, so the generation moves on past it.
  uint32_t generation = (useBin ? dev_.generation : 0) + 1;
  uint8_t head[mptg::kHeaderBytes];
  uint8_t dir[cc::kDirEntryBytes * kOCount];
  uint32_t headerCrc = 0;
  for (;;) {
    std::memset(head, 0, sizeof(head));
    cc::put32(head, cc::kMagicMptg);
    cc::put16(head + 4, cc::kMajor);
    cc::put16(head + 6, 0);
    cc::put32(head + 8, mptg::kHeaderBytes);
    cc::put32(head + 12, nSec);
    cc::put32(head + 16, fileBytes);
    cc::put32(head + 20, generation);
    cc::put64(head + 24, cfg_.cardId);
    head[40] = mptg::kSourceDevice;
    cc::put16(head + 42, cfg_.parserVersion);
    cc::put16(head + 44, mptg::kReadRules);
    cc::put32(head + 48, records);
    cc::put32(head + 52, folders);
    cc::put32(head + 56, albumValues);
    cc::put32(head + 60, artistValues);
    cc::put32(head + 64, (cfg_.producer && cfg_.producer[0]) ? 1u : 0u);
    uint32_t k = 0;
    for (const Sec& x : s) {
      if (!x.on) continue;
      uint8_t* e = dir + cc::kDirEntryBytes * k++;
      std::memset(e, 0, cc::kDirEntryBytes);
      cc::put32(e, x.type);
      cc::put32(e + 4, x.flags);
      cc::put32(e + 8, x.offset);
      cc::put32(e + 12, x.bytes);
      cc::put32(e + 16, x.count);
      cc::put32(e + 20, x.stride);
      cc::put32(e + 24, x.crc);
    }
    headerCrc = cc::crc32(head, mptg::kHeaderBytes);
    headerCrc = cc::crc32(dir, cc::kDirEntryBytes * nSec, headerCrc);
    if (headerCrc != 0) break;
    ++generation;
  }
  cc::put32(head + 32, headerCrc);
  if (!out->write(mptg::kHeaderBytes, dir, cc::kDirEntryBytes * nSec) || !out->write(0, head, mptg::kHeaderBytes) ||
      !out->sync())
    return finish("write");
  const bool closed = fs_.close(out);
  out = nullptr;
  if (!closed) return finish("write");
  tmpMade = false;  // whole: from here the rename's rule decides

  // Into place (2.12.6), then the journals it merged.
  fs_.remove(hidxPath_);
  if (!replace(fs_, names_)) {
    cleanup();
    w->~CompactWork();
    free_(mem);
    Opened o = open();  // the card says what happened
    (void)o;
    res.ok = false;
    res.error = "rename";
    return res;
  }
  res.chunksMerged = chunkN_;
  res.chunksDropped = extraChunks_;
  res.walkMerged = walkRuns_ > 0;
  res.records = records;
  res.folders = folders;
  if (fs_.exists(walkPath_)) fs_.remove(walkPath_);
  if (fs_.exists(jnlPath_)) fs_.remove(jnlPath_);
  cleanup();
  w->~CompactWork();
  free_(mem);
  // The store as the card now is.
  loadDevice();
  chunkN_ = 0;
  extraChunks_ = 0;
  jnlEnd_ = 0;
  jnlTail_ = jnlForeign_ = false;
  if (fs_.exists(jnlPath_)) jnlForeign_ = true;
  walkRuns_ = 0;
  walkSeq_ = 0;
  walkSkew_ = 0;
  walkUnsettled_ = false;
  walkId_ = Identity();
  res.ok = dev_.present;
  if (!res.ok) res.error = "reload";
  return res;
}

}  // namespace tagstore
