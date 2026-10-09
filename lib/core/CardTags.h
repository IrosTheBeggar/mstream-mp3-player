// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardContainer.h"
#include "CardContract.h"

// Tags files, MPTG v1 (docs/METADATA.md 2.6): one format for both producers.
// /.mstream/tags-<gen>.bin is the transfer software's (source 2; 3 for the
// device as the sync agent): its records and its ledger (ORIG, OSTR).
// /.player/tags.bin is the device's (source 1), with its private sections
// after the contract's (3.3.2). One record per file: the file's tags as read
// by part 5's rules, in a 72-byte row (RECS) and a string run (2.6.5) in
// STRS; the folders in FOLD; a path-hash index (HIDX).
//
// Reading, for the builder (N2) and the walk (N5): Walker streams a file in
// its canonical order (2.6.8), a folder and then its records, then the next
// folder in pre-order, and checks everything 2.4.3 asks as it goes, with
// fixed memory whatever the file holds: FOLD and RECS front to back, STRS
// as two runs at once (the folder names, then the records' names and runs;
// 2.4.4's order lets each be read front to back, and their two CRCs combine
// into the section's), HIDX at the end. A failure found mid-stream makes
// the file absent from then on (Step::Bad); the CRCs are known at the end
// (Step::End), so a caller merging as it reads must be ready to restart
// without the file (3.4.1). File reads the same file at random (a record,
// a path, a lookup through HIDX), for the console and the tests, once a
// walk has passed. Writing: write() makes a file from records given in any
// order (the host tests, and the future sync agent; the device's own
// tags.bin streams through N4's compaction instead).
namespace cardcontract {
namespace mptg {

constexpr uint32_t kHeaderBytes = 72;
constexpr uint32_t kFold = fourcc("FOLD");
constexpr uint32_t kRecs = fourcc("RECS");
constexpr uint32_t kHidx = fourcc("HIDX");
constexpr uint32_t kOrig = fourcc("ORIG");
constexpr uint32_t kOstr = fourcc("OSTR");
constexpr uint32_t kFoldStride = 16;
constexpr uint32_t kRecsStride = 72;
constexpr uint32_t kHidxStride = 12;
constexpr uint32_t kOrigStride = 80;
// The sections in 2.6.2's order: Container's indexes.
enum Sec : uint32_t { kSecFold = 0, kSecRecs, kSecStrs, kSecHidx, kSecOrig, kSecOstr, kSecCount };
extern const FormatSpec kSpec;

constexpr uint8_t kSourceDevice = 1;
constexpr uint8_t kSourceTransfer = 2;
constexpr uint8_t kSourceSyncAgent = 3;
constexpr uint16_t kReadRules = 1;  // part 5's reading rules, this version

// FOLD flags.
constexpr uint32_t kFolderOwned = 1;  // the producer created it (sources 2 and 3)
constexpr uint32_t kFolderThumb = 2;  // /.mstream/thumbs has this album folder's thumbnail
constexpr uint32_t kNoParent = 0xFFFFFFFFu;

// RECS flags.
constexpr uint16_t kCompilationMask = 3;  // 0 not said, 1 yes, 2 said no
constexpr uint16_t kHasRgTrack = 1u << 2;
constexpr uint16_t kHasRgAlbum = 1u << 3;
constexpr uint16_t kRgFromR128 = 1u << 4;
constexpr uint16_t kNoTags = 1u << 5;
constexpr uint16_t kUnreadable = 1u << 6;
constexpr uint16_t kTruncated = 1u << 7;
constexpr uint16_t kBpmAnalysed = 1u << 8;
constexpr uint16_t kFromApi = 1u << 9;

// RECS known: which fields the producer looked for.
constexpr uint32_t kKnownTitle = 1u << 0;
constexpr uint32_t kKnownArtist = 1u << 1;
constexpr uint32_t kKnownAlbum = 1u << 2;
constexpr uint32_t kKnownAlbumArtist = 1u << 3;
constexpr uint32_t kKnownGenre = 1u << 4;
constexpr uint32_t kKnownComposer = 1u << 5;
constexpr uint32_t kKnownSortNames = 1u << 6;
constexpr uint32_t kKnownMbIds = 1u << 7;
constexpr uint32_t kKnownYear = 1u << 8;
constexpr uint32_t kKnownTrack = 1u << 9;
constexpr uint32_t kKnownDisc = 1u << 10;
constexpr uint32_t kKnownDuration = 1u << 11;
constexpr uint32_t kKnownBpm = 1u << 12;
constexpr uint32_t kKnownCamelot = 1u << 13;
constexpr uint32_t kKnownReplayGain = 1u << 14;
constexpr uint32_t kKnownCompilation = 1u << 15;
constexpr uint32_t kKnownPicture = 1u << 16;
constexpr uint32_t kKnownRules1 = 0x1FFFFu;  // a readable audio file under readRules 1

// RECS container, picMime, picCoding.
constexpr uint8_t kContainerUnknown = 0;
constexpr uint8_t kContainerMp3 = 1;
constexpr uint8_t kContainerFlac = 2;
constexpr uint8_t kContainerOpus = 3;
constexpr uint8_t kContainerNotAudio = 255;
constexpr uint8_t kMimeNone = 0, kMimeJpeg = 1, kMimePng = 2, kMimeOther = 3;
constexpr uint8_t kCodingRaw = 0, kCodingUnsync = 1, kCodingOggBase64 = 2, kCodingApe = 3;

// ORIG originFlags.
constexpr uint8_t kOriginHashSampled = 1;
constexpr uint8_t kOriginVerified = 2;
constexpr uint8_t kOriginAdopted = 4;

// The header's own fields (2.6.1).
struct Info {
  Header frame;
  uint8_t source = 0;
  uint16_t parserVersion = 0;
  uint16_t readRules = 0;
  uint32_t recordCount = 0;
  uint32_t folderCount = 0;
  uint32_t albumValues = 0;
  uint32_t artistValues = 0;
  uint32_t producer = 0;  // STRS offset
};

struct Folder {
  uint32_t parent = kNoParent;
  uint32_t name = 0;  // STRS offset
  uint32_t flags = 0;
  uint32_t firstRecord = 0;
};

// One RECS row, as stored (2.6.4). The enums are raw: read them through
// the accessors below, which apply 2.4.5's table to unknown values.
struct Record {
  uint32_t folder = 0;
  uint32_t name = 0;     // STRS offset
  uint32_t strings = 0;  // STRS offset of the run; 0 none
  uint32_t size = 0;
  uint32_t fatTime = 0;
  uint32_t durationMs = 0;
  uint64_t qfp = 0;
  uint32_t known = 0;
  uint16_t flags = 0;
  uint16_t year = 0;
  uint16_t track = 0;
  uint16_t trackTotal = 0;
  uint16_t disc = 0;
  uint16_t discTotal = 0;
  uint16_t bpm10 = 0;
  int16_t rgTrackGain = 0;
  int16_t rgAlbumGain = 0;
  uint16_t rgTrackPeak = 0;
  uint16_t rgAlbumPeak = 0;
  uint8_t container = 0;
  uint8_t camelot = 0;
  uint32_t picOffset = 0;
  uint32_t picLength = 0;
  uint8_t picType = 0;
  uint8_t picMime = 0;
  uint8_t picCoding = 0;
};
// 2.4.5: container 4-254 reads as 0; camelot above 24 as 0; picMime above 3
// as 3 (other: not decoded); picCoding above 3 as no picture; compilation 3
// as 0 (not said).
uint8_t containerOf(const Record& r);
uint8_t camelotOf(const Record& r);
uint8_t picMimeOf(const Record& r);
bool hasPicture(const Record& r);
uint8_t compilationOf(const Record& r);

// One ORIG row (2.6.6): the ledger, row i for record i.
struct LedgerRow {
  uint32_t serverPath = 0;  // OSTR offset
  uint32_t mstreamId = 0;
  uint32_t albumId = 0;
  uint32_t artistId = 0;
  uint8_t audioHash[16] = {};
  uint8_t fileHash[16] = {};
  uint64_t serverModified = 0;
  uint64_t serverSize = 0;
  uint32_t createdAt = 0;
  uint16_t hashV = 0;
  uint8_t originFlags = 0;
  uint8_t convertedTo = 0;
  uint16_t convertKbps = 0;
};

void encodeFolder(const Folder& f, uint8_t out[kFoldStride]);
void decodeFolder(const uint8_t* in, Folder* f);
void encodeRecord(const Record& r, uint8_t out[kRecsStride]);
void decodeRecord(const uint8_t* in, Record* r);
void encodeLedger(const LedgerRow& l, uint8_t out[kOrigStride]);
void decodeLedger(const uint8_t* in, LedgerRow* l);

// What a reader relies on beyond FOLD, RECS and STRS, which every reader
// reads: those sections' CRCs and rules are checked too.
constexpr uint32_t kUseHidx = 1;    // HIDX (required when there are records)
constexpr uint32_t kUseLedger = 2;  // ORIG and OSTR, when the file has them (the software; the device ignores them)
constexpr uint32_t kUseAll = kUseHidx | kUseLedger;

// The frame and the header (Container::open, then: source 1-3, the counts
// against FOLD and RECS, at least folder 0).
Why openFile(Container& c, Source& src, Info* info);

// About 3.9 KB (its path buffers and a folder stack of 125 levels): a heap
// or PSRAM object, like the scratch and the RunFields it is given.
class Walker {
public:
  enum class Step : uint8_t { Folder, Record, End, Bad };
  // The smallest scratch begin() takes: seven stream buffers of 64 bytes.
  static constexpr uint32_t kMinScratch = 7 * 64;
  // The deepest folder a valid file can hold: "/music/" + its path within
  // 255 bytes, every level at least "x/".
  static constexpr uint32_t kMaxDepth = (kMaxRelPath + 1) / 2;

  // Starts over `src`: checks the frame and the header, reads the
  // producer, the first record and its name, then is ready to walk.
  // `scratch` is split equally between the streams it reads (FOLD, RECS
  // and STRS's two runs; HIDX, and ORIG and OSTR, when used): 8 KB, 2 KB a
  // stream for the device's four, is 3.4.1's budget. `run`, when given,
  // receives each record's run (the run is checked either way). Ok, or why
  // the file is absent.
  Why begin(Source& src, uint32_t uses, uint8_t* scratch, uint32_t scratchBytes, RunFields* run = nullptr);
  Step next();
  Why why() const { return why_; }
  const Info& info() const { return info_; }
  const Container& container() const { return c_; }
  // The header's producer (its first 63 bytes).
  const char* producer() const { return producer_; }

  // After Step::Folder: the folder and its index; path() is its path
  // relative to /music ("" for /music itself), pathHash() its path hash.
  uint32_t folderIndex() const { return curFolder_; }
  const Folder& folder() const { return folder_; }
  uint32_t depth() const { return depth_ ? depth_ - 1 : 0; }
  // After Step::Record: the record, its index, its name, and path() is its
  // path ("Artist/Album/01.mp3"), pathHash() that path's hash.
  uint32_t recordIndex() const { return curRecord_; }
  const Record& record() const { return rec_; }
  const char* name() const { return path_ + nameAt_; }
  size_t nameLength() const { return pathLen_ - nameAt_; }
  const char* path() const { return path_; }
  size_t pathLength() const { return pathLen_; }
  uint64_t pathHash() const { return hash_; }
  // The record's run, parsed (when begin() was given one); the record's
  // ledger row and its server path (kUseLedger and the file has them; the
  // path cut at kMaxRelPath bytes: a server's paths aren't bound by the
  // card's limit, and File::serverPath() reads one whole).
  const RunFields* run() const { return run_; }
  bool hasLedger() const { return ledgerOn_; }
  const LedgerRow& ledger() const { return ledger_; }
  const char* serverPath() const { return serverPath_; }

private:
  struct Level {
    uint64_t hash;   // the path hash of the folder's path
    uint32_t index;
    uint16_t at;     // its name in path_: [at, end)
    uint16_t end;
  };
  Step fail(Why w);
  bool loadRecord();
  Step enterFolder();
  Step emitRecord();
  Step finish();

  Container c_;
  Info info_;
  uint32_t uses_ = 0;
  Stream fold_, recs_, names_, runs_, hidx_, orig_, ostr_;
  RunFields* run_ = nullptr;
  Why why_ = Why::Ok;
  bool done_ = false;
  uint32_t strsSplit_ = 0;  // where the records' run of STRS starts
  // The walk.
  uint32_t nextFolder_ = 0;
  uint32_t curFolder_ = kNoParent;
  uint32_t nextRecord_ = 0;  // the lookahead's index
  uint32_t curRecord_ = kNoParent;
  bool haveLook_ = false;
  Record look_;
  char lookName_[kMaxRelPath + 1];
  size_t lookNameLen_ = 0;
  Folder folder_;
  Record rec_;
  Level stack_[kMaxDepth + 1];
  uint32_t depth_ = 0;  // levels on the stack
  char path_[kMaxRelPath + 2];
  size_t pathLen_ = 0;
  size_t folderLen_ = 0;  // the current folder's path in path_
  size_t nameAt_ = 0;
  uint64_t hash_ = 0;
  char producer_[64];
  // HIDX's check: two order-free digests of (path hash, record) over the
  // records, compared with the same over HIDX's entries.
  uint64_t digestA_ = 0, digestB_ = 0;
  // The ledger.
  bool ledgerOn_ = false;
  LedgerRow ledger_;
  char serverPath_[kMaxRelPath + 1];
};

// A whole check: a walk to the end. Ok, or why the file is absent. Its
// Walker (3.9 KB) is on the caller's stack: the host's tests. On the device
// walk a Walker of your own in PSRAM (app/TagConsole: the loop task's 8 KB
// stack overflowed with this one on it).
Why check(Source& src, uint32_t uses, uint8_t* scratch, uint32_t scratchBytes);

// Random access to a file the caller has checked (a walk that reached
// End): every read is still bounds-checked.
class File {
public:
  static constexpr uint32_t kNotFound = 0xFFFFFFFFu;
  Why open(Source& src);
  const Info& info() const { return info_; }
  const Container& container() const { return c_; }
  bool folder(uint32_t i, Folder* out) const;
  bool record(uint32_t i, Record* out) const;
  bool ledger(uint32_t i, LedgerRow* out) const;
  Why string(uint32_t off, char* out, size_t cap, size_t* len = nullptr) const;
  Why serverPath(uint32_t off, char* out, size_t cap, size_t* len = nullptr) const;
  Why run(uint32_t off, RunFields* out) const;
  // A folder's or a record's path relative to /music, through FOLD's
  // parents. False: a broken chain, or it doesn't fit `cap`.
  bool folderPath(uint32_t i, char* out, size_t cap, size_t* len = nullptr) const;
  bool recordPath(uint32_t i, char* out, size_t cap, size_t* len = nullptr) const;
  // The record of `rel` through HIDX: a binary search on its path hash, each
  // hit's full path compared. kNotFound when it isn't there or the file has
  // no HIDX.
  uint32_t find(const char* rel, size_t len) const;

private:
  Container c_;
  Info info_;
};

// ---------------------------------------------------------------------------
// Writing (2.6.8's order, from records in any order).
// ---------------------------------------------------------------------------
struct LedgerIn {
  const char* serverPath = "";  // mStream's filepath; "" for an owned file with no server track
  LedgerRow row;                // its serverPath offset is the writer's
};

struct RecordIn {
  const char* path = nullptr;  // relative to /music, names as the card stores them
  Record rec;                  // folder, name and strings are the writer's
  // The fields of the run, each already through 2.3.6 (FieldBuilder: lists
  // joined with U+001F, no NUL); nullptr or "" absent.
  const char* fields[kRunFields] = {};
  const LedgerIn* ledger = nullptr;  // sources 2 and 3; nullptr: a zero row
};

// A folder that must be in FOLD with these flags: an OWNED folder (kept even
// when empty), or a record's ancestor with THUMB. Every ancestor of a
// record is added anyway; any other folder is refused (2.6.3).
struct FolderIn {
  const char* path = "";  // relative to /music; "" is /music
  uint32_t flags = 0;
};

// The device's private sections, after the contract's (3.3.2).
struct ExtraSection {
  uint32_t type = 0;
  uint32_t flags = 0;
  uint32_t count = 0;
  uint32_t stride = 0;
  const uint8_t* data = nullptr;
  uint32_t bytes = 0;
};

struct Meta {
  uint32_t generation = 0;
  uint64_t cardId = 0;
  uint16_t minor = 0;
  uint8_t source = kSourceTransfer;
  uint16_t parserVersion = 0;
  uint16_t readRules = kReadRules;
  const char* producer = "";
};

struct Written {
  uint32_t fileBytes = 0;
  uint32_t headerCrc = 0;
  uint32_t folderCount = 0;
  uint32_t recordCount = 0;
};

// Writes a tags file: the folders every record needs (and `folders`), FOLD
// in pre-order, RECS in canonical order, STRS in 2.6.8's order, HIDX when
// there are records, ORIG and OSTR when the source is 2 or 3 and there are
// records, then `extra`. False with `error` when the input breaks a rule (a
// path, a duplicate, a field with a NUL...) or the sink fails.
bool write(Sink& out, const Meta& meta, const RecordIn* recs, size_t n, const FolderIn* folders, size_t nf,
           const ExtraSection* extra, size_t ne, Written* written, const char** error);

}  // namespace mptg
}  // namespace cardcontract
