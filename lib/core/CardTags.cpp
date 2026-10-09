// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CardTags.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cardcontract {
namespace mptg {

namespace {

const SectionSpec kSections[kSecCount] = {
    {kFold, kFoldStride, true},  {kRecs, kRecsStride, true},  {kStrs, 0, true},
    {kHidx, kHidxStride, false}, {kOrig, kOrigStride, false}, {kOstr, 0, false},
};

// splitmix64's finaliser: HIDX's order-free digests.
uint64_t mix(uint64_t x) {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x;
}
uint64_t digestA(uint64_t hash, uint32_t rec) { return mix(hash ^ mix(rec + 0x9E3779B97F4A7C15ull)); }
uint64_t digestB(uint64_t hash, uint32_t rec) { return mix((hash + 0x632BE59BD9B4E019ull) ^ mix(rec * 0xD1B54A32D192ED03ull + 1)); }

// The prefix of s[0, n) that ends on a whole code point (the copy of a
// field cut at its limit may end inside one).
size_t wholeCodePoints(const char* s, size_t n) {
  size_t k = n;
  while (k > 0 && (static_cast<uint8_t>(s[k - 1]) & 0xC0) == 0x80) --k;
  if (k == 0) return n;  // no lead byte: not UTF-8, keep the bytes
  const uint8_t lead = static_cast<uint8_t>(s[k - 1]);
  const size_t len = lead < 0x80 ? 1 : (lead >= 0xF0 ? 4 : (lead >= 0xE0 ? 3 : (lead >= 0xC0 ? 2 : 1)));
  return (k - 1) + len <= n ? n : k - 1;
}

// Field f's slot in a RunFields' buffer.
size_t slotOf(uint32_t f) {
  size_t at = 1;
  for (uint32_t k = 0; k < f; ++k) at += fieldMax(k) + 1;
  return at;
}

}  // namespace

const FormatSpec kSpec = {kMagicMptg, kHeaderBytes, kSections, kSecCount};

uint8_t containerOf(const Record& r) {
  return (r.container >= 4 && r.container <= 254) ? kContainerUnknown : r.container;
}
uint8_t camelotOf(const Record& r) { return r.camelot > 24 ? 0 : r.camelot; }
uint8_t picMimeOf(const Record& r) { return r.picMime > 3 ? kMimeOther : r.picMime; }
bool hasPicture(const Record& r) { return r.picCoding <= 3 && r.picOffset != 0; }
uint8_t compilationOf(const Record& r) {
  const uint8_t c = static_cast<uint8_t>(r.flags & kCompilationMask);
  return c == 3 ? 0 : c;
}

void encodeFolder(const Folder& f, uint8_t out[kFoldStride]) {
  put32(out, f.parent);
  put32(out + 4, f.name);
  put32(out + 8, f.flags);
  put32(out + 12, f.firstRecord);
}

void decodeFolder(const uint8_t* in, Folder* f) {
  f->parent = get32(in);
  f->name = get32(in + 4);
  f->flags = get32(in + 8);
  f->firstRecord = get32(in + 12);
}

void encodeRecord(const Record& r, uint8_t out[kRecsStride]) {
  std::memset(out, 0, kRecsStride);
  put32(out, r.folder);
  put32(out + 4, r.name);
  put32(out + 8, r.strings);
  put32(out + 12, r.size);
  put32(out + 16, r.fatTime);
  put32(out + 20, r.durationMs);
  put64(out + 24, r.qfp);
  put32(out + 32, r.known);
  put16(out + 36, r.flags);
  put16(out + 38, r.year);
  put16(out + 40, r.track);
  put16(out + 42, r.trackTotal);
  put16(out + 44, r.disc);
  put16(out + 46, r.discTotal);
  put16(out + 48, r.bpm10);
  put16(out + 50, static_cast<uint16_t>(r.rgTrackGain));
  put16(out + 52, static_cast<uint16_t>(r.rgAlbumGain));
  put16(out + 54, r.rgTrackPeak);
  put16(out + 56, r.rgAlbumPeak);
  out[58] = r.container;
  out[59] = r.camelot;
  put32(out + 60, r.picOffset);
  put32(out + 64, r.picLength);
  out[68] = r.picType;
  out[69] = r.picMime;
  out[70] = r.picCoding;
}

void decodeRecord(const uint8_t* in, Record* r) {
  r->folder = get32(in);
  r->name = get32(in + 4);
  r->strings = get32(in + 8);
  r->size = get32(in + 12);
  r->fatTime = get32(in + 16);
  r->durationMs = get32(in + 20);
  r->qfp = get64(in + 24);
  r->known = get32(in + 32);
  r->flags = get16(in + 36);
  r->year = get16(in + 38);
  r->track = get16(in + 40);
  r->trackTotal = get16(in + 42);
  r->disc = get16(in + 44);
  r->discTotal = get16(in + 46);
  r->bpm10 = get16(in + 48);
  r->rgTrackGain = static_cast<int16_t>(get16(in + 50));
  r->rgAlbumGain = static_cast<int16_t>(get16(in + 52));
  r->rgTrackPeak = get16(in + 54);
  r->rgAlbumPeak = get16(in + 56);
  r->container = in[58];
  r->camelot = in[59];
  r->picOffset = get32(in + 60);
  r->picLength = get32(in + 64);
  r->picType = in[68];
  r->picMime = in[69];
  r->picCoding = in[70];
}

void encodeLedger(const LedgerRow& l, uint8_t out[kOrigStride]) {
  std::memset(out, 0, kOrigStride);
  put32(out, l.serverPath);
  put32(out + 4, l.mstreamId);
  put32(out + 8, l.albumId);
  put32(out + 12, l.artistId);
  std::memcpy(out + 16, l.audioHash, 16);
  std::memcpy(out + 32, l.fileHash, 16);
  put64(out + 48, l.serverModified);
  put64(out + 56, l.serverSize);
  put32(out + 64, l.createdAt);
  put16(out + 68, l.hashV);
  out[70] = l.originFlags;
  out[71] = l.convertedTo;
  put16(out + 72, l.convertKbps);
}

void decodeLedger(const uint8_t* in, LedgerRow* l) {
  l->serverPath = get32(in);
  l->mstreamId = get32(in + 4);
  l->albumId = get32(in + 8);
  l->artistId = get32(in + 12);
  std::memcpy(l->audioHash, in + 16, 16);
  std::memcpy(l->fileHash, in + 32, 16);
  l->serverModified = get64(in + 48);
  l->serverSize = get64(in + 56);
  l->createdAt = get32(in + 64);
  l->hashV = get16(in + 68);
  l->originFlags = in[70];
  l->convertedTo = in[71];
  l->convertKbps = get16(in + 72);
}

Why openFile(Container& c, Source& src, Info* info) {
  *info = Info();
  uint8_t th[kHeaderBytes - kCommonHeaderBytes];
  const Why w = c.open(src, kSpec, th);
  if (w != Why::Ok) return w;
  info->frame = c.header();
  info->source = th[0];
  info->parserVersion = get16(th + 2);
  info->readRules = get16(th + 4);
  info->recordCount = get32(th + 8);
  info->folderCount = get32(th + 12);
  info->albumValues = get32(th + 16);
  info->artistValues = get32(th + 20);
  info->producer = get32(th + 24);
  if (info->source < kSourceDevice || info->source > kSourceSyncAgent) return Why::Enum;
  if (c.section(kSecFold).count != info->folderCount || c.section(kSecRecs).count != info->recordCount)
    return Why::Counts;
  if (info->folderCount == 0) return Why::FoldRoot;  // folder 0, /music, is always there
  return Why::Ok;
}

// ---------------------------------------------------------------------------
// Walker
// ---------------------------------------------------------------------------
Walker::Step Walker::fail(Why w) {
  if (why_ == Why::Ok) why_ = w;
  return Step::Bad;
}

Why Walker::begin(Source& src, uint32_t uses, uint8_t* scratch, uint32_t scratchBytes, RunFields* run) {
  uses_ = uses;
  run_ = run;
  why_ = Why::Ok;
  done_ = false;
  nextFolder_ = 0;
  curFolder_ = kNoParent;
  nextRecord_ = 0;
  curRecord_ = kNoParent;
  haveLook_ = false;
  depth_ = 0;
  pathLen_ = folderLen_ = nameAt_ = 0;
  path_[0] = 0;
  hash_ = kMusicHash;
  producer_[0] = 0;
  serverPath_[0] = 0;
  digestA_ = digestB_ = 0;
  ledgerOn_ = false;
  folder_ = Folder();
  rec_ = Record();
  ledger_ = LedgerRow();
  if (run_) run_->clear();
  if (!scratch || scratchBytes < kMinScratch) {
    why_ = Why::Io;
    return why_;
  }
  why_ = openFile(c_, src, &info_);
  if (why_ != Why::Ok) return why_;

  const Section& fold = c_.section(kSecFold);
  const Section& recs = c_.section(kSecRecs);
  const Section& strs = c_.section(kSecStrs);
  const Section& hidx = c_.section(kSecHidx);
  const Section& orig = c_.section(kSecOrig);
  const Section& ostr = c_.section(kSecOstr);
  if (uses & kUseHidx) {
    if (info_.recordCount > 0 && !hidx.present()) return why_ = Why::Missing;
    if (hidx.present() && hidx.count != info_.recordCount) return why_ = Why::Hidx;
  }
  if ((uses & kUseLedger) && orig.present()) {
    if (!ostr.present()) return why_ = Why::Missing;
    if (orig.count != info_.recordCount) return why_ = Why::Orig;
    ledgerOn_ = true;
  }
  if (strs.bytes == 0) return why_ = Why::String;  // byte 0 is the empty string

  // The streams' buffers: equal parts of the scratch.
  const uint32_t streams = 4 + ((uses & kUseHidx) && hidx.present() ? 1 : 0) + (ledgerOn_ ? 2 : 0);
  const uint32_t part = scratchBytes / streams;
  uint8_t* buf = scratch;
  auto next = [&]() {
    uint8_t* b = buf;
    buf += part;
    return b;
  };
  fold_.begin(&src, fold.offset, fold.offset + fold.bytes, next(), part);
  recs_.begin(&src, recs.offset, recs.offset + recs.bytes, next(), part);
  uint8_t* namesBuf = next();
  uint8_t* runsBuf = next();
  if ((uses & kUseHidx) && hidx.present()) hidx_.begin(&src, hidx.offset, hidx.offset + hidx.bytes, next(), part);
  if (ledgerOn_) {
    orig_.begin(&src, orig.offset, orig.offset + orig.bytes, next(), part);
    ostr_.begin(&src, ostr.offset, ostr.offset + ostr.bytes, next(), part);
  }

  // STRS is read as two runs: the producer and the folder names, then from
  // the first record's name on, the records' names and runs (2.4.4).
  uint8_t row[kRecsStride];
  if (info_.recordCount > 0) {
    if (!recs_.read(row, kRecsStride) || !recs_.skipTo(recs_.position() + recs.stride - kRecsStride))
      return why_ = Why::Io;
    decodeRecord(row, &look_);
    if (look_.name == 0) return why_ = Why::Name;
    if (look_.name >= strs.bytes) return why_ = Why::String;
    strsSplit_ = look_.name;
  } else {
    strsSplit_ = strs.bytes;
  }
  names_.begin(&src, strs.offset, strs.offset + strsSplit_, namesBuf, part);
  runs_.begin(&src, strs.offset + strsSplit_, strs.offset + strs.bytes, runsBuf, part);
  uint8_t b;
  if (!names_.readByte(&b) || b != 0) return why_ = Why::String;
  if (info_.producer) {
    if (info_.producer >= strs.bytes) return why_ = Why::String;
    if (info_.producer >= strsSplit_) return why_ = Why::StringOrder;
    size_t len = 0;
    if (!names_.skipTo(strs.offset + info_.producer) || !names_.readString(producer_, sizeof(producer_), &len))
      return why_ = Why::String;
    if (len >= sizeof(producer_)) producer_[wholeCodePoints(producer_, sizeof(producer_) - 1)] = 0;
  }
  if (ledgerOn_ && (!ostr_.readByte(&b) || b != 0)) return why_ = Why::String;
  if (info_.recordCount > 0) {
    nextRecord_ = 1;
    // The first record's name starts the records' run.
    if (!runs_.readString(lookName_, sizeof(lookName_), &lookNameLen_)) return why_ = Why::String;
    if (lookNameLen_ > kMaxRelPath) return why_ = Why::PathLength;
    if (!validName(lookName_, lookNameLen_)) return why_ = Why::Name;
    if (look_.folder >= info_.folderCount) return why_ = Why::RecsFolder;
    haveLook_ = true;
  }
  return why_;
}

bool Walker::loadRecord() {
  haveLook_ = false;
  if (nextRecord_ >= info_.recordCount) return true;
  const Section& recs = c_.section(kSecRecs);
  const Section& strs = c_.section(kSecStrs);
  uint8_t row[kRecsStride];
  if (!recs_.read(row, kRecsStride) || !recs_.skipTo(recs_.position() + recs.stride - kRecsStride)) {
    fail(Why::Io);
    return false;
  }
  decodeRecord(row, &look_);
  if (look_.folder >= info_.folderCount) return fail(Why::RecsFolder), false;
  if (look_.folder < rec_.folder) return fail(Why::RecsOrder), false;
  if (look_.name == 0) return fail(Why::Name), false;
  if (look_.name >= strs.bytes) return fail(Why::String), false;
  if (strs.offset + look_.name < runs_.position()) return fail(Why::StringOrder), false;
  if (!runs_.skipTo(strs.offset + look_.name) || !runs_.readString(lookName_, sizeof(lookName_), &lookNameLen_))
    return fail(Why::String), false;
  if (lookNameLen_ > kMaxRelPath) return fail(Why::PathLength), false;
  if (!validName(lookName_, lookNameLen_)) return fail(Why::Name), false;
  if (look_.folder == rec_.folder && compareNames(lookName_, lookNameLen_, path_ + nameAt_, pathLen_ - nameAt_) <= 0)
    return fail(Why::RecsOrder), false;
  ++nextRecord_;
  haveLook_ = true;
  return true;
}

Walker::Step Walker::next() {
  if (why_ != Why::Ok) return Step::Bad;
  if (done_) return Step::End;
  if (curFolder_ != kNoParent && haveLook_ && look_.folder == curFolder_) return emitRecord();
  if (nextFolder_ < info_.folderCount) {
    if (haveLook_ && look_.folder < nextFolder_) return fail(Why::RecsOrder);
    return enterFolder();
  }
  if (haveLook_) return fail(Why::RecsOrder);
  return finish();
}

Walker::Step Walker::enterFolder() {
  const Section& fold = c_.section(kSecFold);
  const Section& strs = c_.section(kSecStrs);
  const uint32_t i = nextFolder_;
  uint8_t row[kFoldStride];
  if (!fold_.read(row, kFoldStride) || !fold_.skipTo(fold_.position() + fold.stride - kFoldStride)) return fail(Why::Io);
  Folder f;
  decodeFolder(row, &f);
  if (i == 0) {
    if (f.parent != kNoParent || f.name != 0) return fail(Why::FoldRoot);
    stack_[0] = Level{kMusicHash, 0, 0, 0};
    depth_ = 1;
    folderLen_ = 0;
    nameAt_ = 0;
  } else {
    if (f.parent >= i) return fail(Why::FoldParent);
    // Pop to the parent, which must be on the stack (the folder before, or
    // one of its ancestors); the level popped just above it is the previous
    // sibling, whose name is still in path_.
    const Level* prev = nullptr;
    while (depth_ > 0 && stack_[depth_ - 1].index != f.parent) prev = &stack_[--depth_];
    if (depth_ == 0) return fail(Why::FoldParent);
    if (f.name == 0) return fail(Why::Name);
    if (f.name >= strs.bytes) return fail(Why::String);
    if (f.name >= strsSplit_ || strs.offset + f.name < names_.position()) return fail(Why::StringOrder);
    char name[kMaxRelPath + 1];
    size_t len = 0;
    if (!names_.skipTo(strs.offset + f.name) || !names_.readString(name, sizeof(name), &len)) return fail(Why::String);
    if (len > kMaxRelPath) return fail(Why::PathLength);
    if (!validName(name, len)) return fail(Why::Name);
    const Level& parent = stack_[depth_ - 1];
    const size_t at = parent.end ? parent.end + 1u : 0u;
    if (at + len > kMaxRelPath || depth_ > kMaxDepth) return fail(Why::PathLength);
    if (prev && compareNames(name, len, path_ + prev->at, prev->end - prev->at) <= 0) return fail(Why::FoldOrder);
    if (parent.end) path_[parent.end] = '/';
    std::memcpy(path_ + at, name, len);
    const uint64_t h = fnv1a64(name, len, fnv1a64("/", 1, parent.hash));
    stack_[depth_] = Level{h, i, static_cast<uint16_t>(at), static_cast<uint16_t>(at + len)};
    ++depth_;
    folderLen_ = at + len;
    nameAt_ = at;
  }
  const uint32_t emitted = curRecord_ == kNoParent ? 0 : curRecord_ + 1;
  if (f.firstRecord != emitted) return fail(Why::FirstRecord);
  path_[folderLen_] = 0;
  pathLen_ = folderLen_;
  hash_ = stack_[depth_ - 1].hash;
  folder_ = f;
  curFolder_ = i;
  ++nextFolder_;
  return Step::Folder;
}

Walker::Step Walker::emitRecord() {
  const Section& strs = c_.section(kSecStrs);
  rec_ = look_;
  const size_t at = folderLen_ ? folderLen_ + 1 : 0;
  if (at + lookNameLen_ > kMaxRelPath) return fail(Why::PathLength);
  if (folderLen_) path_[folderLen_] = '/';
  std::memcpy(path_ + at, lookName_, lookNameLen_);
  pathLen_ = at + lookNameLen_;
  path_[pathLen_] = 0;
  nameAt_ = at;
  hash_ = fnv1a64(lookName_, lookNameLen_, fnv1a64("/", 1, stack_[depth_ - 1].hash));
  curRecord_ = curRecord_ == kNoParent ? 0 : curRecord_ + 1;

  // The run, read in place from the records' run of STRS.
  if (run_) run_->clear();
  if (rec_.strings) {
    if (rec_.strings >= strs.bytes) return fail(Why::String);
    if (strs.offset + rec_.strings < runs_.position()) return fail(Why::StringOrder);
    uint8_t n = 0;
    if (!runs_.skipTo(strs.offset + rec_.strings) || !runs_.readByte(&n)) return fail(Why::String);
    for (uint32_t f = 0; f < n; ++f) {
      size_t len = 0;
      if (run_ && f < kRunFields) {
        char* slot = run_->text + slotOf(f);
        const size_t max = fieldMax(f);
        if (!runs_.readString(slot, max + 1, &len)) return fail(Why::String);
        size_t keep = len;
        if (len > max) {
          keep = wholeCodePoints(slot, max);
          run_->cut = true;
        }
        slot[keep] = 0;
        run_->offset[f] = static_cast<uint16_t>(slotOf(f));
        run_->length[f] = static_cast<uint16_t>(keep);
      } else if (!runs_.readString(nullptr, 0, &len)) {
        return fail(Why::String);
      }
    }
    if (run_) run_->n = n;
  }

  // The ledger row and its server path.
  if (ledgerOn_) {
    const Section& orig = c_.section(kSecOrig);
    const Section& ostr = c_.section(kSecOstr);
    uint8_t row[kOrigStride];
    if (!orig_.read(row, kOrigStride) || !orig_.skipTo(orig_.position() + orig.stride - kOrigStride))
      return fail(Why::Io);
    decodeLedger(row, &ledger_);
    serverPath_[0] = 0;
    if (ledger_.serverPath) {
      if (ledger_.serverPath >= ostr.bytes) return fail(Why::String);
      if (ostr.offset + ledger_.serverPath < ostr_.position()) return fail(Why::StringOrder);
      size_t len = 0;
      if (!ostr_.skipTo(ostr.offset + ledger_.serverPath) || !ostr_.readString(serverPath_, sizeof(serverPath_), &len))
        return fail(Why::String);
      if (len >= sizeof(serverPath_)) serverPath_[wholeCodePoints(serverPath_, sizeof(serverPath_) - 1)] = 0;
    }
  }

  digestA_ += digestA(hash_, curRecord_);
  digestB_ += digestB(hash_, curRecord_);
  if (!loadRecord()) return Step::Bad;
  return Step::Record;
}

Walker::Step Walker::finish() {
  if (!fold_.drain() || !recs_.drain() || !names_.drain() || !runs_.drain()) return fail(Why::Io);
  if (fold_.crc() != c_.section(kSecFold).crc || recs_.crc() != c_.section(kSecRecs).crc) return fail(Why::SectionCrc);
  const uint32_t strsCrc = crc32Combine(names_.crc(), runs_.crc(), runs_.end() - runs_.start());
  if (strsCrc != c_.section(kSecStrs).crc) return fail(Why::SectionCrc);
  const Section& hidx = c_.section(kSecHidx);
  if ((uses_ & kUseHidx) && hidx.present()) {
    uint64_t a = 0, b = 0, prevHash = 0;
    uint32_t prevRec = 0;
    for (uint32_t i = 0; i < hidx.count; ++i) {
      uint8_t e[kHidxStride];
      if (!hidx_.read(e, kHidxStride) || !hidx_.skipTo(hidx_.position() + hidx.stride - kHidxStride))
        return fail(Why::Io);
      const uint64_t h = get64(e);
      const uint32_t r = get32(e + 8);
      if (r >= info_.recordCount) return fail(Why::Hidx);
      if (i > 0 && (h < prevHash || (h == prevHash && r <= prevRec))) return fail(Why::Hidx);
      a += digestA(h, r);
      b += digestB(h, r);
      prevHash = h;
      prevRec = r;
    }
    if (!hidx_.drain()) return fail(Why::Io);
    if (hidx_.crc() != hidx.crc) return fail(Why::SectionCrc);
    // Strictly increasing pairs, as many as records, whose digests equal
    // the records' (path hash, index) pairs': each record once, with its
    // own hash.
    if (a != digestA_ || b != digestB_) return fail(Why::Hidx);
  }
  if (ledgerOn_) {
    if (!orig_.drain() || !ostr_.drain()) return fail(Why::Io);
    if (orig_.crc() != c_.section(kSecOrig).crc || ostr_.crc() != c_.section(kSecOstr).crc)
      return fail(Why::SectionCrc);
  }
  done_ = true;
  return Step::End;
}

Why check(Source& src, uint32_t uses, uint8_t* scratch, uint32_t scratchBytes) {
  Walker w;
  Why why = w.begin(src, uses, scratch, scratchBytes);
  if (why != Why::Ok) return why;
  for (;;) {
    const Walker::Step s = w.next();
    if (s == Walker::Step::End) return Why::Ok;
    if (s == Walker::Step::Bad) return w.why();
  }
}

// ---------------------------------------------------------------------------
// File
// ---------------------------------------------------------------------------
Why File::open(Source& src) { return openFile(c_, src, &info_); }

bool File::folder(uint32_t i, Folder* out) const {
  const Section& s = c_.section(kSecFold);
  if (i >= s.count) return false;
  uint8_t row[kFoldStride];
  if (!c_.source()->read(s.offset + i * s.stride, row, kFoldStride)) return false;
  decodeFolder(row, out);
  return true;
}

bool File::record(uint32_t i, Record* out) const {
  const Section& s = c_.section(kSecRecs);
  if (i >= s.count) return false;
  uint8_t row[kRecsStride];
  if (!c_.source()->read(s.offset + i * s.stride, row, kRecsStride)) return false;
  decodeRecord(row, out);
  return true;
}

bool File::ledger(uint32_t i, LedgerRow* out) const {
  const Section& s = c_.section(kSecOrig);
  if (!s.present() || i >= s.count) return false;
  uint8_t row[kOrigStride];
  if (!c_.source()->read(s.offset + i * s.stride, row, kOrigStride)) return false;
  decodeLedger(row, out);
  return true;
}

Why File::string(uint32_t off, char* out, size_t cap, size_t* len) const {
  return readStringAt(*c_.source(), c_.section(kSecStrs), off, out, cap, len);
}

Why File::serverPath(uint32_t off, char* out, size_t cap, size_t* len) const {
  if (!c_.section(kSecOstr).present()) return Why::Missing;
  return readStringAt(*c_.source(), c_.section(kSecOstr), off, out, cap, len);
}

Why File::run(uint32_t off, RunFields* out) const {
  out->clear();
  if (off == 0) return Why::Ok;
  const Section& strs = c_.section(kSecStrs);
  if (off >= strs.bytes) return Why::String;
  uint8_t n = 0;
  if (!c_.source()->read(strs.offset + off, &n, 1)) return Why::Io;
  uint32_t at = off + 1;
  char buf[FieldBuilder::kListMax + 2];
  for (uint32_t f = 0; f < n; ++f) {
    size_t len = 0;
    const Why w = readStringAt(*c_.source(), strs, at, buf, sizeof(buf), &len);
    if (w != Why::Ok) return w;
    if (f < kRunFields) out->set(f, buf, len < sizeof(buf) - 1 ? len : sizeof(buf) - 1);
    if (len >= sizeof(buf) - 1 && f < kRunFields) out->cut = true;
    at += static_cast<uint32_t>(len) + 1;
  }
  out->n = n;
  return Why::Ok;
}

bool File::folderPath(uint32_t i, char* out, size_t cap, size_t* len) const {
  uint32_t chain[Walker::kMaxDepth + 1];
  uint32_t depth = 0;
  Folder f;
  while (i != 0) {
    if (depth > Walker::kMaxDepth || !folder(i, &f) || f.parent >= i) return false;
    chain[depth++] = f.name;
    i = f.parent;
  }
  size_t n = 0;
  for (uint32_t k = depth; k-- > 0;) {
    if (n) {
      if (n + 1 >= cap) return false;
      out[n++] = '/';
    }
    size_t l = 0;
    if (n >= cap || string(chain[k], out + n, cap - n, &l) != Why::Ok || n + l >= cap) return false;
    n += l;
  }
  if (cap == 0) return false;
  out[n] = 0;
  if (len) *len = n;
  return true;
}

bool File::recordPath(uint32_t i, char* out, size_t cap, size_t* len) const {
  Record r;
  size_t n = 0;
  if (!record(i, &r) || !folderPath(r.folder, out, cap, &n)) return false;
  if (n) {
    if (n + 1 >= cap) return false;
    out[n++] = '/';
  }
  size_t l = 0;
  if (string(r.name, out + n, cap - n, &l) != Why::Ok || n + l >= cap) return false;
  if (len) *len = n + l;
  return true;
}

uint32_t File::find(const char* rel, size_t len) const {
  const Section& s = c_.section(kSecHidx);
  if (!s.present() || s.count == 0) return kNotFound;
  const uint64_t h = pathHash(rel, len);
  auto entry = [&](uint32_t i, uint64_t* hash, uint32_t* rec) {
    uint8_t e[kHidxStride];
    if (!c_.source()->read(s.offset + i * s.stride, e, kHidxStride)) return false;
    *hash = get64(e);
    *rec = get32(e + 8);
    return true;
  };
  // The first entry whose hash is at least h.
  uint32_t lo = 0, hi = s.count;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    uint64_t eh;
    uint32_t er;
    if (!entry(mid, &eh, &er)) return kNotFound;
    if (eh < h)
      lo = mid + 1;
    else
      hi = mid;
  }
  char buf[kMaxRelPath + 2];
  for (uint32_t i = lo; i < s.count; ++i) {
    uint64_t eh;
    uint32_t er;
    if (!entry(i, &eh, &er) || eh != h) break;
    size_t l = 0;
    if (recordPath(er, buf, sizeof(buf), &l) && l == len && std::memcmp(buf, rel, len) == 0) return er;
  }
  return kNotFound;
}

// ---------------------------------------------------------------------------
// The writer
// ---------------------------------------------------------------------------
namespace {

struct FolderLess {
  bool operator()(const std::string& a, const std::string& b) const {
    return compareFolderPaths(a.data(), a.size(), b.data(), b.size()) < 0;
  }
};

std::string parentOf(const std::string& p) {
  const size_t k = p.rfind('/');
  return k == std::string::npos ? std::string() : p.substr(0, k);
}

std::string lastName(const std::string& p) {
  const size_t k = p.rfind('/');
  return k == std::string::npos ? p : p.substr(k + 1);
}

// A field as 2.3.6 leaves it: no control character but a list's
// separators, values non-empty, unique, at most 255 bytes, a list of at
// most 16 and 1,023 bytes.
bool fieldConforms(uint32_t f, const char* s) {
  const size_t n = std::strlen(s);
  if (n == 0) return true;  // absent
  if (n > fieldMax(f)) return false;
  std::set<std::string> seen;
  size_t start = 0;
  uint32_t values = 0;
  for (size_t i = 0; i <= n; ++i) {
    const uint8_t c = i < n ? static_cast<uint8_t>(s[i]) : 0;
    if (i == n || c == static_cast<uint8_t>(kSeparator)) {
      if (i < n && !isListField(f)) return false;
      const size_t len = i - start;
      if (len == 0 || len > FieldBuilder::kValueMax) return false;
      if (!seen.insert(std::string(s + start, len)).second) return false;
      ++values;
      start = i + 1;
    } else if (c < 0x20 || c == 0x7F) {
      return false;
    }
  }
  return values <= FieldBuilder::kListValues;
}

void splitInto(const char* s, std::set<std::string>* out) {
  if (!s || !*s) return;
  const char* p = s;
  for (;;) {
    const char* e = std::strchr(p, kSeparator);
    out->insert(e ? std::string(p, static_cast<size_t>(e - p)) : std::string(p));
    if (!e) break;
    p = e + 1;
  }
}

class VecWriter {
public:
  uint32_t add(const std::string& s) {
    if (s.empty()) return 0;
    const uint32_t off = static_cast<uint32_t>(bytes.size());
    bytes.insert(bytes.end(), s.begin(), s.end());
    bytes.push_back(0);
    return off;
  }
  std::vector<uint8_t> bytes{0};
};

}  // namespace

bool write(Sink& out, const Meta& meta, const RecordIn* recs, size_t n, const FolderIn* folders, size_t nf,
           const ExtraSection* extra, size_t ne, Written* written, const char** error) {
  auto bad = [&](const char* e) {
    if (error) *error = e;
    return false;
  };
  if (meta.source < kSourceDevice || meta.source > kSourceSyncAgent) return bad("source");
  if (n > 0x7FFFFFFF) return bad("too many records");

  // Every ancestor of a record, then the folders asked for (an OWNED one
  // with its ancestors; a THUMB one must be a record's ancestor).
  std::map<std::string, uint32_t, FolderLess> fset;
  fset[std::string()] = 0;
  std::set<std::string> recPaths;
  for (size_t i = 0; i < n; ++i) {
    if (!recs[i].path) return bad("record path");
    const std::string p = recs[i].path;
    if (!validRelPath(p.data(), p.size())) return bad("record path");
    if (!recPaths.insert(p).second) return bad("duplicate record path");
    for (std::string a = parentOf(p); !a.empty(); a = parentOf(a)) fset[a];
  }
  for (size_t i = 0; i < nf; ++i) {
    const std::string p = folders[i].path ? folders[i].path : "";
    if (!p.empty() && !validRelPath(p.data(), p.size())) return bad("folder path");
    if (!fset.count(p) && !(folders[i].flags & kFolderOwned)) return bad("a folder neither owned nor a record's ancestor");
  }
  for (size_t i = 0; i < nf; ++i) {
    const std::string p = folders[i].path ? folders[i].path : "";
    fset[p] |= folders[i].flags;
    for (std::string a = parentOf(p); !a.empty(); a = parentOf(a)) fset[a];
  }
  for (const std::string& p : recPaths)
    if (fset.count(p)) return bad("a path both a file and a folder");

  std::vector<std::string> fpaths;
  std::map<std::string, uint32_t> findex;
  for (const auto& kv : fset) {
    findex[kv.first] = static_cast<uint32_t>(fpaths.size());
    fpaths.push_back(kv.first);
  }

  // Records in canonical order: by folder index, then name bytes.
  struct R {
    size_t in;
    uint32_t folder;
    std::string name;
  };
  std::vector<R> order;
  order.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const std::string p = recs[i].path;
    order.push_back(R{i, findex[parentOf(p)], lastName(p)});
    for (uint32_t f = 0; f < kRunFields; ++f)
      if (recs[i].fields[f] && !fieldConforms(f, recs[i].fields[f])) return bad("a field breaks 2.3.6");
    if (recs[i].ledger && meta.source == kSourceDevice) return bad("a ledger row in a source 1 file");
  }
  std::sort(order.begin(), order.end(), [](const R& a, const R& b) {
    if (a.folder != b.folder) return a.folder < b.folder;
    return compareNames(a.name.data(), a.name.size(), b.name.data(), b.name.size()) < 0;
  });

  // STRS in 2.6.8's order; FOLD; RECS.
  VecWriter strs;
  const uint32_t producer = strs.add(meta.producer ? meta.producer : "");
  std::vector<uint8_t> fold(fpaths.size() * kFoldStride);
  std::vector<uint32_t> firstRecord(fpaths.size() + 1, 0);
  for (const R& r : order) ++firstRecord[r.folder + 1];
  for (size_t i = 1; i <= fpaths.size(); ++i) firstRecord[i] += firstRecord[i - 1];
  for (size_t i = 0; i < fpaths.size(); ++i) {
    Folder f;
    f.parent = i == 0 ? kNoParent : findex[parentOf(fpaths[i])];
    f.name = i == 0 ? 0 : strs.add(lastName(fpaths[i]));
    f.flags = fset[fpaths[i]];
    f.firstRecord = firstRecord[i];
    encodeFolder(f, fold.data() + i * kFoldStride);
  }
  std::vector<uint8_t> recsBytes(order.size() * kRecsStride);
  std::vector<std::pair<uint64_t, uint32_t>> hidx;
  std::set<std::string> albums, artists;
  uint8_t run[kRunMax];
  for (size_t k = 0; k < order.size(); ++k) {
    const RecordIn& in = recs[order[k].in];
    Record r = in.rec;
    r.folder = order[k].folder;
    r.name = strs.add(order[k].name);
    const size_t runLen = encodeRun(in.fields, run, sizeof(run));
    r.strings = runLen ? static_cast<uint32_t>(strs.bytes.size()) : 0;
    strs.bytes.insert(strs.bytes.end(), run, run + runLen);
    encodeRecord(r, recsBytes.data() + k * kRecsStride);
    hidx.emplace_back(pathHash(in.path), static_cast<uint32_t>(k));
    if (in.fields[kAlbum] && in.fields[kAlbum][0]) albums.insert(in.fields[kAlbum]);
    splitInto(in.fields[kArtist], &artists);
    splitInto(in.fields[kAlbumArtist], &artists);
  }
  std::sort(hidx.begin(), hidx.end());
  std::vector<uint8_t> hidxBytes(hidx.size() * kHidxStride);
  for (size_t k = 0; k < hidx.size(); ++k) {
    put64(hidxBytes.data() + k * kHidxStride, hidx[k].first);
    put32(hidxBytes.data() + k * kHidxStride + 8, hidx[k].second);
  }
  if (strs.bytes.size() > 0xFFFFFFFFull) return bad("too big");

  // The ledger, row k for record k.
  const bool ledger = meta.source != kSourceDevice && !order.empty();
  VecWriter ostr;
  std::vector<uint8_t> origBytes;
  if (ledger) {
    origBytes.resize(order.size() * kOrigStride);
    for (size_t k = 0; k < order.size(); ++k) {
      const LedgerIn* in = recs[order[k].in].ledger;
      LedgerRow row = in ? in->row : LedgerRow();
      row.serverPath = ostr.add(in && in->serverPath ? in->serverPath : "");
      encodeLedger(row, origBytes.data() + k * kOrigStride);
    }
  }

  uint8_t th[kHeaderBytes - kCommonHeaderBytes] = {};
  th[0] = meta.source;
  put16(th + 2, meta.parserVersion);
  put16(th + 4, meta.readRules);
  put32(th + 8, static_cast<uint32_t>(order.size()));
  put32(th + 12, static_cast<uint32_t>(fpaths.size()));
  put32(th + 16, static_cast<uint32_t>(albums.size()));
  put32(th + 20, static_cast<uint32_t>(artists.size()));
  put32(th + 24, producer);

  SectionOut s[kMaxSections];
  const uint8_t* data[kMaxSections];
  uint32_t ns = 0;
  auto section = [&](uint32_t type, uint32_t flags, uint32_t count, uint32_t stride, const std::vector<uint8_t>& b) {
    s[ns].type = type;
    s[ns].flags = flags;
    s[ns].count = count;
    s[ns].stride = stride;
    s[ns].bytes = static_cast<uint32_t>(b.size());
    data[ns] = b.data();
    ++ns;
  };
  section(kFold, kSectionRequired, static_cast<uint32_t>(fpaths.size()), kFoldStride, fold);
  section(kRecs, kSectionRequired, static_cast<uint32_t>(order.size()), kRecsStride, recsBytes);
  section(kStrs, kSectionRequired, 0, 0, strs.bytes);
  if (!order.empty()) section(kHidx, 0, static_cast<uint32_t>(order.size()), kHidxStride, hidxBytes);
  if (ledger) {
    section(kOrig, 0, static_cast<uint32_t>(order.size()), kOrigStride, origBytes);
    section(kOstr, 0, 0, 0, ostr.bytes);
  }
  if (ns + ne > kMaxSections) return bad("too many sections");
  for (size_t i = 0; i < ne; ++i) {
    s[ns].type = extra[i].type;
    s[ns].flags = extra[i].flags;
    s[ns].count = extra[i].count;
    s[ns].stride = extra[i].stride;
    s[ns].bytes = extra[i].bytes;
    data[ns] = extra[i].data;
    ++ns;
  }

  FileMeta fm;
  fm.magic = kMagicMptg;
  fm.minor = meta.minor;
  fm.generation = meta.generation;
  fm.cardId = meta.cardId;
  ContainerWriter cw;
  if (!cw.begin(out, fm, th, kHeaderBytes, s, ns)) return bad("layout");
  for (uint32_t i = 0; i < ns; ++i)
    if (s[i].bytes && !cw.write(data[i], s[i].bytes)) return bad("sink");
  Written w;
  if (!cw.finish(&w.fileBytes, &w.headerCrc)) return bad("sink");
  w.folderCount = static_cast<uint32_t>(fpaths.size());
  w.recordCount = static_cast<uint32_t>(order.size());
  if (written) *written = w;
  return true;
}

}  // namespace mptg
}  // namespace cardcontract
