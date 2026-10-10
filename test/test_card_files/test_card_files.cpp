// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the card contract's binary files (lib/core/
// CardContainer, CardTags, CardManifest, CardAutoDj; docs/METADATA.md 2.4-
// 2.6, 2.12.5, 2.13, 2.17): the C++ writers make the shared golden files
// (test/fixtures/card/golden, made by tools/card_fixtures.py, a second
// implementation) byte for byte from the library descriptions; the readers
// read them back field for field; every hardening file (each check of 2.4.3
// broken under valid CRCs) reads as absent for the reason it breaks, and
// the newer minor as present; truncation at every byte, every flipped bit
// and a resealed-CRC fuzz pass make every file absent (or, for padding and
// sections a reader doesn't use, leave it present) with no crash and no
// endless loop; a large synthetic library round-trips through the walker's
// smallest buffers. Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../support/CardFixtures.h"
#include "CardAutoDj.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "CardManifest.h"
#include "CardTags.h"
#include "ThumbCache.h"

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;
namespace msmf = cardcontract::msmf;
namespace mspd = cardcontract::mspd;
namespace mpdj = cardcontract::mpdj;
using minijson::Value;
using Bytes = std::vector<uint8_t>;

namespace {

class VecSink : public cc::Sink {
public:
  bool write(uint32_t offset, const void* data, uint32_t n) override {
    if (offset + n > b.size()) b.resize(offset + n);
    if (n) std::memcpy(b.data() + offset, data, n);
    return true;
  }
  Bytes b;
};

Bytes golden(const std::string& name) {
  const std::string s = cardfixtures::bytes("golden/" + name);
  return Bytes(s.begin(), s.end());
}

Value library(const std::string& name) {
  Value v;
  TEST_ASSERT_TRUE_MESSAGE(cardfixtures::json("libraries/" + name + ".json", &v), name.c_str());
  return v;
}

Bytes hexBytes(const std::string& s) {
  Bytes out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
  return out;
}

const char* const kFieldNames[cc::kRunFields] = {"title",     "artist",          "album",     "albumArtist",
                                                 "genre",     "composer",        "titleSort", "artistSort",
                                                 "albumSort", "albumArtistSort", "mbAlbumId", "mbRecordingId"};

// A library description's records as the writer's input (2.3.6 applied to
// the raw values here, TRUNCATED added: the fixture README's rule).
struct TagsInput {
  mptg::Meta meta;
  std::vector<std::string> paths, serverPaths, folderPaths;
  std::vector<std::array<std::string, cc::kRunFields>> fields;
  std::vector<mptg::RecordIn> recs;
  std::vector<mptg::LedgerIn> ledgers;
  std::vector<bool> hasLedger;
  std::vector<mptg::FolderIn> folders;

  explicit TagsInput(const Value& d) {
    meta.generation = static_cast<uint32_t>(d["generation"].u64());
    meta.cardId = d["cardId"].u64();
    meta.minor = static_cast<uint16_t>(d["minor"].u64());
    meta.source = static_cast<uint8_t>(d["source"].u64());
    meta.parserVersion = static_cast<uint16_t>(d["parserVersion"].u64());
    meta.readRules = static_cast<uint16_t>(d["readRules"].u64(1));
    const Value& rs = d["records"];
    const size_t n = rs.size();
    paths.resize(n);
    serverPaths.resize(n);
    fields.resize(n);
    recs.resize(n);
    ledgers.resize(n);
    hasLedger.resize(n);
    for (size_t i = 0; i < n; ++i) {
      const Value& r = rs[i];
      paths[i] = r["path"].str();
      mptg::Record& x = recs[i].rec;
      x.size = static_cast<uint32_t>(r["size"].u64());
      x.fatTime = static_cast<uint32_t>(r["fatTime"].u64());
      x.durationMs = static_cast<uint32_t>(r["durationMs"].u64());
      x.qfp = r["qfp"].u64();
      x.known = static_cast<uint32_t>(r["known"].u64());
      x.flags = static_cast<uint16_t>(r["flags"].u64());
      x.year = static_cast<uint16_t>(r["year"].u64());
      x.track = static_cast<uint16_t>(r["track"].u64());
      x.trackTotal = static_cast<uint16_t>(r["trackTotal"].u64());
      x.disc = static_cast<uint16_t>(r["disc"].u64());
      x.discTotal = static_cast<uint16_t>(r["discTotal"].u64());
      x.bpm10 = static_cast<uint16_t>(r["bpm10"].u64());
      x.rgTrackGain = static_cast<int16_t>(r["rgTrackGain"].i64());
      x.rgAlbumGain = static_cast<int16_t>(r["rgAlbumGain"].i64());
      x.rgTrackPeak = static_cast<uint16_t>(r["rgTrackPeak"].u64());
      x.rgAlbumPeak = static_cast<uint16_t>(r["rgAlbumPeak"].u64());
      x.container = static_cast<uint8_t>(r["container"].u64());
      x.camelot = static_cast<uint8_t>(r["camelot"].u64());
      const Value& pic = r["picture"];
      x.picOffset = static_cast<uint32_t>(pic["offset"].u64());
      x.picLength = static_cast<uint32_t>(pic["length"].u64());
      x.picType = static_cast<uint8_t>(pic["type"].u64());
      x.picMime = static_cast<uint8_t>(pic["mime"].u64());
      x.picCoding = static_cast<uint8_t>(pic["coding"].u64());
      bool truncated = false;
      for (int f = 0; f < cc::kRunFields; ++f) {
        const Value& vals = r["tags"][kFieldNames[f]];
        cc::FieldBuilder b(cc::isListField(f));
        for (size_t k = 0; k < vals.size(); ++k) b.add(vals[k].str().data(), vals[k].str().size());
        fields[i][f] = b.data();
        truncated = truncated || b.truncated();
      }
      if (truncated) x.flags |= mptg::kTruncated;
      hasLedger[i] = r.has("ledger");
      if (hasLedger[i]) {
        const Value& l = r["ledger"];
        serverPaths[i] = l["serverPath"].str();
        mptg::LedgerRow& row = ledgers[i].row;
        row.mstreamId = static_cast<uint32_t>(l["mstreamId"].u64());
        row.albumId = static_cast<uint32_t>(l["albumId"].u64());
        row.artistId = static_cast<uint32_t>(l["artistId"].u64());
        const Bytes ah = hexBytes(l["audioHash"].str()), fh = hexBytes(l["fileHash"].str());
        if (ah.size() == 16) std::memcpy(row.audioHash, ah.data(), 16);
        if (fh.size() == 16) std::memcpy(row.fileHash, fh.data(), 16);
        row.serverModified = l["serverModified"].u64();
        row.serverSize = l["serverSize"].u64();
        row.createdAt = static_cast<uint32_t>(l["createdAt"].u64());
        row.hashV = static_cast<uint16_t>(l["hashV"].u64());
        row.originFlags = static_cast<uint8_t>(l["originFlags"].u64());
        row.convertedTo = static_cast<uint8_t>(l["convertedTo"].u64());
        row.convertKbps = static_cast<uint16_t>(l["convertKbps"].u64());
      }
    }
    for (size_t i = 0; i < n; ++i) {
      recs[i].path = paths[i].c_str();
      for (int f = 0; f < cc::kRunFields; ++f) recs[i].fields[f] = fields[i][f].c_str();
      if (hasLedger[i]) {
        ledgers[i].serverPath = serverPaths[i].c_str();
        recs[i].ledger = &ledgers[i];
      }
    }
    const Value& fs = d["folders"];
    folderPaths.resize(fs.size());
    folders.resize(fs.size());
    for (size_t i = 0; i < fs.size(); ++i) {
      folderPaths[i] = fs[i]["path"].str();
      folders[i].path = folderPaths[i].c_str();
      folders[i].flags = static_cast<uint32_t>(fs[i]["flags"].u64());
    }
  }

  bool write(VecSink* out, const std::string& producer, const char** error) {
    meta.producer = producer.c_str();
    return mptg::write(*out, meta, recs.data(), recs.size(), folders.data(), folders.size(), nullptr, 0, nullptr, error);
  }
};

// The walk of a tags file, every event, for comparisons.
struct Event {
  bool folder;
  std::string path;
  uint64_t hash;
  mptg::Folder f;
  mptg::Record r;
  std::array<std::string, cc::kRunFields> fields;
  mptg::LedgerRow ledger;
  std::string serverPath;
};

cc::Why walk(const Bytes& file, uint32_t uses, std::vector<Event>* out, uint32_t scratchBytes = 7 * 1024) {
  cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
  std::vector<uint8_t> scratch(scratchBytes);
  std::unique_ptr<cc::RunFields> run(new cc::RunFields());
  mptg::Walker w;
  cc::Why why = w.begin(src, uses, scratch.data(), scratchBytes, run.get());
  if (why != cc::Why::Ok) return why;
  for (;;) {
    const mptg::Walker::Step s = w.next();
    if (s == mptg::Walker::Step::End) return cc::Why::Ok;
    if (s == mptg::Walker::Step::Bad) return w.why();
    Event e;
    e.folder = s == mptg::Walker::Step::Folder;
    e.path.assign(w.path(), w.pathLength());
    e.hash = w.pathHash();
    if (e.folder) {
      e.f = w.folder();
    } else {
      e.r = w.record();
      for (int f = 0; f < cc::kRunFields; ++f) e.fields[f] = w.run()->get(f);
      if (w.hasLedger()) {
        e.ledger = w.ledger();
        e.serverPath = w.serverPath();
      }
    }
    if (out) out->push_back(e);
  }
}

bool sameRecord(const mptg::Record& a, const mptg::Record& b) {
  uint8_t x[mptg::kRecsStride], y[mptg::kRecsStride];
  mptg::Record ca = a, cb = b;
  ca.folder = cb.folder = 0;
  ca.name = cb.name = 0;
  ca.strings = cb.strings = 0;
  mptg::encodeRecord(ca, x);
  mptg::encodeRecord(cb, y);
  return std::memcmp(x, y, sizeof(x)) == 0;
}

// The same library: the same events, offsets aside.
void assertSameWalk(const std::vector<Event>& a, const std::vector<Event>& b) {
  TEST_ASSERT_EQUAL_size_t(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    TEST_ASSERT_EQUAL(a[i].folder, b[i].folder);
    TEST_ASSERT_EQUAL_STRING(a[i].path.c_str(), b[i].path.c_str());
    TEST_ASSERT_TRUE(a[i].hash == b[i].hash);
    if (a[i].folder) {
      TEST_ASSERT_EQUAL_UINT32(a[i].f.parent, b[i].f.parent);
      TEST_ASSERT_EQUAL_UINT32(a[i].f.flags, b[i].f.flags);
      TEST_ASSERT_EQUAL_UINT32(a[i].f.firstRecord, b[i].f.firstRecord);
    } else {
      TEST_ASSERT_TRUE_MESSAGE(sameRecord(a[i].r, b[i].r), a[i].path.c_str());
      for (int f = 0; f < cc::kRunFields; ++f) TEST_ASSERT_EQUAL_STRING(a[i].fields[f].c_str(), b[i].fields[f].c_str());
      TEST_ASSERT_EQUAL_STRING(a[i].serverPath.c_str(), b[i].serverPath.c_str());
    }
  }
}

// Reads any contract file as its format's reader does: Ok, or why it is absent.
cc::Why readAs(const std::string& format, const Bytes& file, bool deviceUses = false) {
  cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
  uint8_t scratch[1024];
  if (format == "MPTG") return mptg::check(src, deviceUses ? 0 : mptg::kUseAll, scratch, sizeof(scratch));
  if (format == "MSMF") {
    msmf::Manifest m;
    return m.open(src, scratch, sizeof(scratch));
  }
  if (format == "MSPD") {
    mspd::Plan p;
    return p.open(src, scratch, sizeof(scratch));
  }
  if (format == "MPDJ") {
    mpdj::Table t;
    const cc::Why w = t.open(src);
    return w != cc::Why::Ok ? w : t.check(mpdj::Table::kUseAll, scratch, sizeof(scratch));
  }
  return cc::Why::Magic;
}

std::string formatOf(const Bytes& b) {
  if (b.size() < 4) return "";
  return std::string(reinterpret_cast<const char*>(b.data()), 4);
}

// The byte ranges a reader doesn't check: the gaps between sections (zero
// padding, which readers ignore), and the sections a device-uses MPTG
// reader skips.
std::vector<bool> unchecked(const Bytes& b, bool deviceUses) {
  std::vector<bool> skip(b.size(), false);
  const uint32_t hb = cc::get32(&b[8]), n = cc::get32(&b[12]);
  uint32_t end = hb + 32 * n;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = &b[hb + 32 * i];
    const uint32_t type = cc::get32(e), off = cc::get32(e + 8), len = cc::get32(e + 12);
    for (uint32_t k = end; k < off; ++k) skip[k] = true;
    if (deviceUses && (type == mptg::kHidx || type == mptg::kOrig || type == mptg::kOstr))
      for (uint32_t k = off; k < off + len; ++k) skip[k] = true;
    end = off + len;
  }
  return skip;
}

// Recomputes every section's CRC (where the directory makes sense) and the
// header's: a mutation then meets the structural checks, not the CRCs.
void reseal(Bytes& b) {
  if (b.size() < 40) return;
  const uint32_t hb = cc::get32(&b[8]), n = cc::get32(&b[12]);
  if (hb < 40 || n > 64 || static_cast<uint64_t>(hb) + 32ull * n > b.size()) return;
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t* e = &b[hb + 32 * i];
    const uint32_t off = cc::get32(e + 8), len = cc::get32(e + 12);
    if (static_cast<uint64_t>(off) + len <= b.size()) cc::put32(e + 24, cc::crc32(b.data() + off, len));
  }
  cc::put32(&b[32], 0);
  cc::put32(&b[32], cc::crc32(b.data(), hb + 32 * n));
}

const char* const kGoldenBinaries[] = {"tags-0000002a.bin",  "tags-device.bin",  "tags-00000001.bin", "manifest.bin",
                                       "manifest-min.bin",   "pending.bin",      "autodj-00000029.bin",
                                       "autodj-small.bin"};

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// The goldens: the C++ writers make them byte for byte.
// ---------------------------------------------------------------------------
void test_golden_tags_files() {
  for (const char* name : {"tags-0000002a", "tags-device", "tags-00000001"}) {
    const Value d = library(name);
    TagsInput in(d);
    VecSink out;
    const char* error = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(in.write(&out, d["producer"].str(), &error), error ? error : name);
    const Bytes want = golden(d["file"].str());
    TEST_ASSERT_TRUE_MESSAGE(!want.empty(), name);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(want.size(), out.b.size(), name);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want.data(), out.b.data(), want.size(), name);
  }
}

void test_golden_tags_writer_is_order_free() {
  const Value d = library("tags-0000002a");
  TagsInput in(d);
  std::mt19937 rng(7);
  std::shuffle(in.recs.begin(), in.recs.end(), rng);
  std::shuffle(in.folders.begin(), in.folders.end(), rng);
  VecSink out;
  TEST_ASSERT_TRUE(in.write(&out, d["producer"].str(), nullptr));
  TEST_ASSERT_TRUE(out.b == golden("tags-0000002a.bin"));
}

void test_golden_autodj_files() {
  for (const char* name : {"autodj-00000029", "autodj-small"}) {
    const Value d = library(name);
    const Value& rs = d["rows"];
    const uint32_t dim = static_cast<uint32_t>(d["dim"].u64());
    std::vector<mpdj::RowIn> rows(rs.size());
    std::vector<std::vector<uint64_t>> paths(rs.size());
    std::vector<std::vector<float>> emb(rs.size());
    std::vector<std::string> songs(rs.size());
    for (size_t i = 0; i < rs.size(); ++i) {
      const Bytes h = hexBytes(rs[i]["hash"].str());
      std::memcpy(rows[i].hash, h.data(), 16);
      for (size_t k = 0; k < rs[i]["paths"].size(); ++k) paths[i].push_back(cc::pathHash(rs[i]["paths"][k].c_str()));
      for (size_t k = 0; k < rs[i]["embedding"].size(); ++k)
        emb[i].push_back(static_cast<float>(rs[i]["embedding"][k].num()));
      songs[i] = rs[i]["songKey"].str();
      rows[i].paths = paths[i].data();
      rows[i].pathCount = static_cast<uint32_t>(paths[i].size());
      rows[i].bpm10 = static_cast<uint16_t>(rs[i]["bpm10"].u64());
      rows[i].camelot = static_cast<uint8_t>(rs[i]["camelot"].u64());
      rows[i].flags = static_cast<uint8_t>(rs[i]["flags"].u64());
      const std::string& ak = rs[i]["artistNameKey"].str();
      rows[i].artistKey = ak.empty() ? 0 : static_cast<uint32_t>(cc::fnv1a64(ak.data(), ak.size()));
      rows[i].songKey = songs[i].c_str();
      rows[i].embedding = emb[i].data();
    }
    mpdj::TableIn in;
    in.generation = static_cast<uint32_t>(d["generation"].u64());
    in.cardId = d["cardId"].u64();
    in.k = static_cast<uint32_t>(d["k"].u64());
    in.builtTime = static_cast<uint32_t>(d["builtTime"].u64());
    in.tagsGeneration = static_cast<uint32_t>(d["tagsGeneration"].u64());
    in.modelId = d["modelId"].c_str();
    in.modelVersion = d["modelVersion"].c_str();
    in.metric = d["metric"].c_str();
    in.license = d["license"].c_str();
    in.attribution = d["attribution"].c_str();
    in.dim = dim;
    in.rows = rows.data();
    in.rowCount = static_cast<uint32_t>(rows.size());
    VecSink out;
    uint8_t sig[16];
    const char* error = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(mpdj::write(out, in, sig, nullptr, nullptr, &error), error ? error : name);
    const Bytes want = golden(d["file"].str());
    TEST_ASSERT_EQUAL_size_t_MESSAGE(want.size(), out.b.size(), name);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want.data(), out.b.data(), want.size(), name);
  }
}

msmf::CompIn compFrom(const Bytes& file) {
  cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
  msmf::CompIn c;
  if (formatOf(file) == "MPTG") {
    cc::Container ct;
    mptg::Info info;
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(mptg::openFile(ct, src, &info)));
    c.kind = cc::kMagicMptg;
    c.generation = info.frame.generation;
    c.fileBytes = info.frame.fileBytes;
    c.headerCrc = info.frame.headerCrc;
  } else {
    mpdj::Table t;
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
    c.kind = cc::kMagicMpdj;
    c.generation = t.frame().generation;
    c.fileBytes = t.frame().fileBytes;
    c.headerCrc = t.frame().headerCrc;
    std::memcpy(c.key, t.selectionSig(), 16);
  }
  return c;
}

void test_golden_manifests() {
  for (const char* name : {"manifest", "manifest-min"}) {
    const Value d = library(name);
    std::vector<msmf::CompIn> comps;
    for (size_t i = 0; i < d["companions"].size(); ++i) comps.push_back(compFrom(golden(d["companions"][i]["file"].str())));
    std::vector<std::string> roots;
    for (size_t i = 0; i < d["roots"].size(); ++i) roots.push_back(d["roots"][i].str());
    std::vector<const char*> rp;
    for (auto& r : roots) rp.push_back(r.c_str());
    msmf::ManifestIn in;
    in.generation = static_cast<uint32_t>(d["generation"].u64());
    in.cardId = d["cardId"].u64();
    in.commitTime = static_cast<uint32_t>(d["commitTime"].u64());
    in.flags = static_cast<uint32_t>(d["flags"].u64());
    in.commitId = d["commitId"].u64();
    std::string uuid = d["serverInstance"].str();
    uuid.erase(std::remove(uuid.begin(), uuid.end(), '-'), uuid.end());
    const Bytes inst = hexBytes(uuid);
    if (inst.size() == 16) std::memcpy(in.serverInstance, inst.data(), 16);
    in.producer = d["producer"].c_str();
    in.serverRevision = d["serverRevision"].c_str();
    in.baseGeneration = static_cast<uint32_t>(d["baseGeneration"].u64());
    in.serverUrlKey = d.has("serverUrl") ? cc::serverUrlKey(d["serverUrl"].c_str()) : d["serverUrlKey"].u64();
    in.comps = comps.data();
    in.compCount = static_cast<uint32_t>(comps.size());
    in.roots = rp.data();
    in.rootCount = static_cast<uint32_t>(rp.size());
    VecSink out;
    const char* error = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(msmf::write(out, in, nullptr, nullptr, &error), error ? error : name);
    const Bytes want = golden(d["file"].str());
    TEST_ASSERT_EQUAL_size_t_MESSAGE(want.size(), out.b.size(), name);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want.data(), out.b.data(), want.size(), name);
  }
}

void test_golden_plan() {
  const Value d = library("pending");
  std::vector<mspd::OpIn> ops(d["ops"].size());
  std::vector<std::string> paths(ops.size());
  for (size_t i = 0; i < ops.size(); ++i) {
    const std::string& op = d["ops"][i]["op"].str();
    ops[i].op = op == "write" ? mspd::kOpWrite : op == "delete" ? mspd::kOpDelete : mspd::kOpFolder;
    paths[i] = d["ops"][i]["path"].str();
    ops[i].path = paths[i].c_str();
    ops[i].expectedSize = static_cast<uint32_t>(d["ops"][i]["expectedSize"].u64());
  }
  mspd::PlanIn in;
  in.generation = static_cast<uint32_t>(d["generation"].u64());
  in.cardId = d["cardId"].u64();
  in.runId = d["runId"].u64();
  in.baseGeneration = static_cast<uint32_t>(d["baseGeneration"].u64());
  in.ops = ops.data();
  in.opCount = static_cast<uint32_t>(ops.size());
  VecSink out;
  TEST_ASSERT_TRUE(mspd::write(out, in, nullptr, nullptr, nullptr));
  TEST_ASSERT_TRUE(out.b == golden("pending.bin"));
}

void test_golden_thumbnail() {
  // MPTH from the description's synthetic picture: the device's thumbfile
  // header, then the two slots.
  const Value d = library("B1F7E69F");
  thumbfile::Header h;
  h.pathHash = cc::pathHash(d["folder"].c_str());
  h.sourceBytes = static_cast<uint32_t>(d["sourceBytes"].u64());
  Bytes out(thumbfile::kFileBytes);
  thumbfile::write(h, out.data());
  size_t at = thumbfile::kHeaderBytes;
  for (int size : {40, 96})
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) {
        const uint8_t r = static_cast<uint8_t>(x * 255 / (size - 1)), g = static_cast<uint8_t>(y * 255 / (size - 1)),
                      b = static_cast<uint8_t>((x + y) * 255 / (2 * size - 2));
        const uint16_t v = static_cast<uint16_t>((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        out[at++] = static_cast<uint8_t>(v >> 8);
        out[at++] = static_cast<uint8_t>(v);
      }
  TEST_ASSERT_TRUE(out == golden(d["file"].str()));
  thumbfile::Header back;
  TEST_ASSERT_TRUE(thumbfile::read(out.data(), out.size(), &back));
  TEST_ASSERT_TRUE(back.pathHash == 0xB1F7E69FBD466B59ull);
}

// ---------------------------------------------------------------------------
// Reading the goldens back.
// ---------------------------------------------------------------------------
void test_tags_read_back() {
  const Value d = library("tags-0000002a");
  TagsInput in(d);
  const Bytes file = golden("tags-0000002a.bin");
  std::vector<Event> ev;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(walk(file, mptg::kUseAll, &ev)));
  std::map<std::string, size_t> byPath;
  for (size_t i = 0; i < in.paths.size(); ++i) byPath[in.paths[i]] = i;
  size_t records = 0;
  const Event* prev = nullptr;
  std::vector<std::string> folders;
  for (const Event& e : ev) {
    TEST_ASSERT_TRUE(e.hash == cc::pathHash(e.path.c_str()));
    if (e.folder) {
      folders.push_back(e.path);
      continue;
    }
    if (prev) TEST_ASSERT_TRUE(cc::compareFilePaths(prev->path.c_str(), e.path.c_str()) < 0);  // canonical order
    prev = &e;
    ++records;
    auto it = byPath.find(e.path);
    TEST_ASSERT_TRUE_MESSAGE(it != byPath.end(), e.path.c_str());
    const size_t i = it->second;
    TEST_ASSERT_TRUE_MESSAGE(sameRecord(in.recs[i].rec, e.r), e.path.c_str());
    for (int f = 0; f < cc::kRunFields; ++f) TEST_ASSERT_EQUAL_STRING(in.fields[i][f].c_str(), e.fields[f].c_str());
    TEST_ASSERT_EQUAL_STRING(in.serverPaths[i].c_str(), e.serverPath.c_str());
    TEST_ASSERT_EQUAL_UINT32(in.ledgers[i].row.mstreamId, e.ledger.mstreamId);
    TEST_ASSERT_EQUAL_MEMORY(in.ledgers[i].row.audioHash, e.ledger.audioHash, 16);
    TEST_ASSERT_TRUE(in.ledgers[i].row.serverSize == e.ledger.serverSize);
  }
  TEST_ASSERT_EQUAL_size_t(in.paths.size(), records);
  for (size_t i = 1; i < folders.size(); ++i) TEST_ASSERT_TRUE(cc::compareFolderPaths(folders[i - 1].c_str(), folders[i].c_str()) < 0);
  // Folder flags: THUMB on the album, OWNED on the empty ones (and their
  // parent is in the table without it).
  std::map<std::string, uint32_t> flags;
  for (const Event& e : ev)
    if (e.folder) flags[e.path] = e.f.flags;
  TEST_ASSERT_EQUAL_UINT32(mptg::kFolderThumb, flags["Artist/Album"]);
  TEST_ASSERT_EQUAL_UINT32(mptg::kFolderOwned, flags["Owned Empty"]);
  TEST_ASSERT_EQUAL_UINT32(mptg::kFolderOwned, flags["Owned Parent/Owned Child"]);
  TEST_ASSERT_EQUAL_UINT32(0, flags["Owned Parent"]);
  TEST_ASSERT_EQUAL_size_t(1, flags.count(""));
  // The 2.3.6 cuts arrived as TRUNCATED.
  for (const Event& e : ev)
    if (!e.folder && (e.path == "A-/z.flac" || e.path == "B/w.opus" || e.path == "a/v.mp3"))
      TEST_ASSERT_TRUE(e.r.flags & mptg::kTruncated);
  // The header.
  cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
  mptg::File f;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(f.open(src)));
  TEST_ASSERT_EQUAL_UINT8(2, f.info().source);
  TEST_ASSERT_EQUAL_UINT32(42, f.info().frame.generation);
  TEST_ASSERT_TRUE(f.info().frame.cardId == 0x5EEDC0DE0A1B2C3Dull);
  TEST_ASSERT_EQUAL_UINT32(in.paths.size(), f.info().recordCount);
  // Random access: each path through HIDX, its path back, its ledger.
  for (size_t i = 0; i < in.paths.size(); ++i) {
    const uint32_t r = f.find(in.paths[i].data(), in.paths[i].size());
    TEST_ASSERT_TRUE_MESSAGE(r != mptg::File::kNotFound, in.paths[i].c_str());
    char path[300];
    size_t len = 0;
    TEST_ASSERT_TRUE(f.recordPath(r, path, sizeof(path), &len));
    TEST_ASSERT_EQUAL_STRING(in.paths[i].c_str(), path);
    mptg::Record rec;
    TEST_ASSERT_TRUE(f.record(r, &rec));
    cc::RunFields run;
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(f.run(rec.strings, &run)));
    for (int k = 0; k < cc::kRunFields; ++k) TEST_ASSERT_EQUAL_STRING(in.fields[i][k].c_str(), run.get(k));
    mptg::LedgerRow l;
    TEST_ASSERT_TRUE(f.ledger(r, &l));
    char sp[300];
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(f.serverPath(l.serverPath, sp, sizeof(sp))));
    TEST_ASSERT_EQUAL_STRING(in.serverPaths[i].c_str(), sp);
  }
  TEST_ASSERT_EQUAL_UINT32(mptg::File::kNotFound, f.find("Artist/Album/03.mp3", 19));
  // A path whose hash is in HIDX but whose bytes differ: NFC and NFD.
  const std::string nfd = "Cafe\xCC\x81/Album/01 - Title.mp3", nfc = "Caf\xC3\xA9/Album/01 - Title.mp3";
  TEST_ASSERT_TRUE(f.find(nfd.data(), nfd.size()) != f.find(nfc.data(), nfc.size()));
}

void test_tags_enums_read_as_the_table_says() {
  mptg::Record r;
  r.container = 7;
  TEST_ASSERT_EQUAL_UINT8(mptg::kContainerUnknown, mptg::containerOf(r));
  r.container = 255;
  TEST_ASSERT_EQUAL_UINT8(255, mptg::containerOf(r));
  r.camelot = 25;
  TEST_ASSERT_EQUAL_UINT8(0, mptg::camelotOf(r));
  r.camelot = 24;
  TEST_ASSERT_EQUAL_UINT8(24, mptg::camelotOf(r));
  r.picMime = 9;
  TEST_ASSERT_EQUAL_UINT8(mptg::kMimeOther, mptg::picMimeOf(r));
  r.picOffset = 10;
  r.picCoding = 4;
  TEST_ASSERT_FALSE(mptg::hasPicture(r));
  r.picCoding = 3;
  TEST_ASSERT_TRUE(mptg::hasPicture(r));
  r.flags = 3;
  TEST_ASSERT_EQUAL_UINT8(0, mptg::compilationOf(r));
  r.flags = 2;
  TEST_ASSERT_EQUAL_UINT8(2, mptg::compilationOf(r));
  // Every field through the row and back.
  mptg::Record a;
  a.folder = 1;
  a.name = 2;
  a.strings = 3;
  a.size = 4;
  a.fatTime = 5;
  a.durationMs = 6;
  a.qfp = 0x0102030405060708ull;
  a.known = 0x1FFFF;
  a.flags = 0xFFFF;
  a.year = 1999;
  a.track = 7;
  a.trackTotal = 8;
  a.disc = 9;
  a.discTotal = 10;
  a.bpm10 = 1210;
  a.rgTrackGain = -679;
  a.rgAlbumGain = -32768;
  a.rgTrackPeak = 65535;
  a.rgAlbumPeak = 1;
  a.container = 3;
  a.camelot = 24;
  a.picOffset = 0xDEADBEEF;
  a.picLength = 77;
  a.picType = 3;
  a.picMime = 1;
  a.picCoding = 2;
  uint8_t row[mptg::kRecsStride];
  mptg::encodeRecord(a, row);
  TEST_ASSERT_EQUAL_UINT8(0, row[71]);  // reserved
  mptg::Record b;
  mptg::decodeRecord(row, &b);
  uint8_t again[mptg::kRecsStride];
  mptg::encodeRecord(b, again);
  TEST_ASSERT_EQUAL_MEMORY(row, again, sizeof(row));
  TEST_ASSERT_EQUAL_INT16(-32768, b.rgAlbumGain);
  TEST_ASSERT_EQUAL_UINT32(0xDEADBEEF, cc::get32(row + 60));
  TEST_ASSERT_EQUAL_UINT16(1210, cc::get16(row + 48));
}

void test_tags_device_and_empty() {
  std::vector<Event> ev;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(walk(golden("tags-device.bin"), mptg::kUseAll, &ev)));
  size_t records = 0;
  for (const Event& e : ev) records += e.folder ? 0 : 1;
  TEST_ASSERT_EQUAL_size_t(4, records);
  // No ledger in a device file, whatever the reader uses.
  for (const Event& e : ev) TEST_ASSERT_EQUAL_STRING("", e.serverPath.c_str());
  ev.clear();
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(walk(golden("tags-00000001.bin"), mptg::kUseAll, &ev)));
  TEST_ASSERT_EQUAL_size_t(2, ev.size());  // /music and the OWNED empty folder
  TEST_ASSERT_TRUE(ev[0].folder && ev[0].path.empty());
  TEST_ASSERT_TRUE(ev[1].folder && ev[1].path == "Owned Empty" && ev[1].f.flags == mptg::kFolderOwned);
  const Bytes empty = golden("tags-00000001.bin");
  cc::MemSource src(empty.data(), static_cast<uint32_t>(empty.size()));
  mptg::File f;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(f.open(src)));
  TEST_ASSERT_FALSE(f.container().section(mptg::kSecHidx).present());
  TEST_ASSERT_FALSE(f.container().section(mptg::kSecOrig).present());
  TEST_ASSERT_EQUAL_UINT32(mptg::File::kNotFound, f.find("x.mp3", 5));
}

void test_manifest_read_back() {
  const Bytes man = golden("manifest.bin");
  cc::MemSource src(man.data(), static_cast<uint32_t>(man.size()));
  uint8_t scratch[256];
  msmf::Manifest m;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(m.open(src, scratch, sizeof(scratch))));
  TEST_ASSERT_EQUAL_UINT32(42, m.frame().generation);
  TEST_ASSERT_EQUAL_UINT32(msmf::kFinal, m.flags());
  TEST_ASSERT_TRUE(m.commitId() == 0x0DDBA11CAFEF00D5ull);
  TEST_ASSERT_TRUE(m.serverUrlKey() == 0xED901EA3EE763AC7ull);
  const uint8_t inst[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                            0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  TEST_ASSERT_EQUAL_MEMORY(inst, m.serverInstance(), 16);
  TEST_ASSERT_EQUAL_STRING("mstream-terminal 0.13.0", m.producer());
  TEST_ASSERT_EQUAL_STRING("tags-0000002a.bin", m.tagsName());
  TEST_ASSERT_TRUE(m.hasAutoDj());
  TEST_ASSERT_EQUAL_STRING("autodj-00000029.bin", m.autoDjName());
  TEST_ASSERT_EQUAL_UINT32(2, m.rootCount());
  char root[64];
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(m.root(0, root, sizeof(root))));
  TEST_ASSERT_EQUAL_STRING("Lib A", root);  // sorted by the writer
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(m.root(1, root, sizeof(root))));
  TEST_ASSERT_EQUAL_STRING("Lib B", root);
  // The companions are the root's.
  const Bytes tags = golden("tags-0000002a.bin"), dj = golden("autodj-00000029.bin"), dev = golden("tags-device.bin");
  cc::MemSource ts(tags.data(), static_cast<uint32_t>(tags.size()));
  cc::Container tc;
  mptg::Info ti;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(mptg::openFile(tc, ts, &ti)));
  TEST_ASSERT_TRUE(msmf::companionMatches(m.tags(), ti.frame, ti.source, nullptr));
  cc::MemSource ds(dj.data(), static_cast<uint32_t>(dj.size()));
  mpdj::Table t;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(ds)));
  TEST_ASSERT_TRUE(msmf::companionMatches(m.autoDj(), t.frame(), 0, t.selectionSig()));
  uint8_t otherSig[16] = {};
  TEST_ASSERT_FALSE(msmf::companionMatches(m.autoDj(), t.frame(), 0, otherSig));
  // Another file, or a device's records under the same numbers: not it.
  cc::MemSource vs(dev.data(), static_cast<uint32_t>(dev.size()));
  cc::Container vc;
  mptg::Info vi;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(mptg::openFile(vc, vs, &vi)));
  TEST_ASSERT_FALSE(msmf::companionMatches(m.tags(), vi.frame, vi.source, nullptr));
  cc::Header forged = ti.frame;
  TEST_ASSERT_FALSE(msmf::companionMatches(m.tags(), forged, 1, nullptr));  // source 1
  forged.headerCrc ^= 1;
  TEST_ASSERT_FALSE(msmf::companionMatches(m.tags(), forged, 2, nullptr));
  // The minimal root: a checkpoint, no MPDJ, no LIBR.
  const Bytes mm = golden("manifest-min.bin");
  cc::MemSource ms(mm.data(), static_cast<uint32_t>(mm.size()));
  msmf::Manifest m2;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(m2.open(ms, scratch, sizeof(scratch))));
  TEST_ASSERT_FALSE(m2.hasAutoDj());
  TEST_ASSERT_EQUAL_UINT32(0, m2.flags());
  TEST_ASSERT_EQUAL_UINT32(0, m2.rootCount());
  TEST_ASSERT_TRUE(m2.serverUrlKey() == 0);
  // The two as candidates: generation 42 beats 1.
  const cc::Election e = cc::elect(m2.candidate(), m.candidate());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(cc::Pick::Tmp), static_cast<int>(e.pick));
  TEST_ASSERT_FALSE(e.sameCommit);
  TEST_ASSERT_TRUE(cc::elect(m.candidate(), m.candidate()).sameCommit);
}

void test_autodj_read_back() {
  const Value d = library("autodj-00000029");
  const Bytes dj = golden("autodj-00000029.bin");
  cc::MemSource src(dj.data(), static_cast<uint32_t>(dj.size()));
  mpdj::Table t;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
  uint8_t scratch[128];
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.check(mpdj::Table::kUseAll, scratch, sizeof(scratch))));
  TEST_ASSERT_EQUAL_UINT32(6, t.rowCount());
  TEST_ASSERT_EQUAL_UINT32(7, t.pathCount());  // one recording at two paths
  TEST_ASSERT_EQUAL_UINT32(3, t.k());
  TEST_ASSERT_EQUAL_UINT32(2, t.indexBytes());
  char s[64];
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.string(0, s, sizeof(s))));
  TEST_ASSERT_EQUAL_STRING("synthetic-embedding", s);
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.string(3, s, sizeof(s))));
  TEST_ASSERT_EQUAL_STRING("CC0-1.0", s);
  // The rows in canonical-hash order; every path finds its row; the
  // neighbours are 2.13.2's, recomputed here from the description.
  struct R {
    Bytes hash;
    std::vector<float> e;
    std::string song;
    std::vector<std::string> paths;
  };
  std::vector<R> rows;
  for (size_t i = 0; i < d["rows"].size(); ++i) {
    R r;
    r.hash = hexBytes(d["rows"][i]["hash"].str());
    for (size_t k = 0; k < d["rows"][i]["embedding"].size(); ++k)
      r.e.push_back(static_cast<float>(d["rows"][i]["embedding"][k].num()));
    r.song = d["rows"][i]["songKey"].str();
    for (size_t k = 0; k < d["rows"][i]["paths"].size(); ++k) r.paths.push_back(d["rows"][i]["paths"][k].str());
    rows.push_back(r);
  }
  std::sort(rows.begin(), rows.end(), [](const R& a, const R& b) { return a.hash < b.hash; });
  for (uint32_t i = 0; i < rows.size(); ++i) {
    mpdj::Row row;
    TEST_ASSERT_TRUE(t.row(i, &row));
    TEST_ASSERT_EQUAL_MEMORY(rows[i].hash.data(), row.hashPrefix, 8);
    uint64_t smallest = ~0ull;
    for (auto& p : rows[i].paths) {
      const uint64_t h = cc::pathHash(p.c_str());
      smallest = std::min(smallest, h);
      TEST_ASSERT_EQUAL_UINT32(i, t.rowOf(h));
    }
    TEST_ASSERT_TRUE(row.pathHash == smallest);
    std::vector<std::pair<int, uint32_t>> cand;
    for (uint32_t j = 0; j < rows.size(); ++j)
      if (j != i && rows[j].song != rows[i].song)
        cand.emplace_back(-static_cast<int>(mpdj::score(rows[i].e.data(), rows[j].e.data(), 4)), j);
    std::sort(cand.begin(), cand.end());
    mpdj::Neighbour nb[8];
    const uint32_t n = t.neighbours(i, nb, 8);
    TEST_ASSERT_EQUAL_UINT32(std::min<size_t>(3, cand.size()), n);
    for (uint32_t k = 0; k < n; ++k) {
      TEST_ASSERT_EQUAL_UINT32(cand[k].second, nb[k].row);
      TEST_ASSERT_EQUAL_UINT8(-cand[k].first, nb[k].score);
      TEST_ASSERT_TRUE(rows[nb[k].row].song != rows[i].song);  // never the same song
    }
  }
  // Row 0's list: 223, then three at 128 for the last two places, the
  // lower rows first: the K boundary cuts a tie.
  mpdj::Neighbour nb[3];
  TEST_ASSERT_EQUAL_UINT32(3, t.neighbours(0, nb, 3));
  TEST_ASSERT_EQUAL_UINT8(223, nb[0].score);
  TEST_ASSERT_EQUAL_UINT8(128, nb[1].score);
  TEST_ASSERT_EQUAL_UINT8(128, nb[2].score);
  TEST_ASSERT_TRUE(nb[1].row < nb[2].row);
  TEST_ASSERT_EQUAL_UINT32(mpdj::kNoRow, t.rowOf(12345));
  // The small table: fewer rows than K, unused slots dropped.
  const Bytes small = golden("autodj-small.bin");
  cc::MemSource ss(small.data(), static_cast<uint32_t>(small.size()));
  mpdj::Table st;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(st.open(ss)));
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(st.check(mpdj::Table::kUseAll, scratch, sizeof(scratch))));
  TEST_ASSERT_EQUAL_UINT32(1, st.neighbours(0, nb, 3));
  TEST_ASSERT_EQUAL_UINT8(0, nb[0].score);  // a negative cosine
  TEST_ASSERT_EQUAL_UINT32(1, nb[0].row);
}

void test_plan_read_back() {
  const Bytes pb = golden("pending.bin");
  cc::MemSource src(pb.data(), static_cast<uint32_t>(pb.size()));
  uint8_t scratch[128];
  mspd::Plan p;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(p.open(src, scratch, sizeof(scratch))));
  TEST_ASSERT_FALSE(p.stopsSoftware());
  TEST_ASSERT_EQUAL_UINT32(9, p.opCount());
  TEST_ASSERT_TRUE(p.runId() == 0x00C0FFEE00C0FFEEull);
  TEST_ASSERT_EQUAL_UINT32(42, p.baseGeneration());
  const char* want[] = {"A/x.mp3",      "Artist/x.mp3", "A B/Sub",   "New Artist", "New Artist/New Album",
                        "Loose New.mp3", "A/z.mp3",     "A B/y.mp3", "New Artist/New Album/01 - New.mp3"};
  const uint8_t ops[] = {2, 2, 3, 3, 3, 1, 1, 1, 1};
  for (uint32_t i = 0; i < 9; ++i) {
    mspd::Op op;
    TEST_ASSERT_TRUE(p.op(i, &op));
    TEST_ASSERT_EQUAL_UINT8(ops[i], op.op);
    char path[128];
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(p.path(op, path, sizeof(path))));
    TEST_ASSERT_EQUAL_STRING(want[i], path);
    TEST_ASSERT_EQUAL_UINT32(op.op == mspd::kOpWrite ? op.expectedSize : 0, op.expectedSize);
  }
  // As a candidate: pending.tmp of a higher generation wins.
  cc::RootCandidate tmp = p.candidate();
  tmp.generation += 1;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(cc::Pick::Tmp), static_cast<int>(cc::elect(p.candidate(), tmp).pick));
}

// ---------------------------------------------------------------------------
// Hardening (2.17, item 4).
// ---------------------------------------------------------------------------
void test_hardening_files() {
  Value index;
  TEST_ASSERT_TRUE(cardfixtures::json("hardening/index.json", &index));
  TEST_ASSERT_TRUE(index.size() >= 40);
  for (size_t i = 0; i < index.size(); ++i) {
    const Value& e = index[i];
    const std::string file = e["file"].str();
    const std::string s = cardfixtures::bytes("hardening/" + file);
    const Bytes b(s.begin(), s.end());
    TEST_ASSERT_TRUE_MESSAGE(!b.empty(), file.c_str());
    const cc::Why why = readAs(e["format"].str(), b, e["uses"].str() == "device");
    const std::string what = file + ": " + e["check"].str();
    TEST_ASSERT_EQUAL_STRING_MESSAGE(e["why"].c_str(), cc::whyName(why), what.c_str());
    if (e.has("same")) {
      std::vector<Event> a, g;
      TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(walk(b, mptg::kUseAll, &a)));
      TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(walk(golden(e["same"].str()), mptg::kUseAll, &g)));
      assertSameWalk(g, a);
    }
    if (file == "mspd-op-9.bin") {
      cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
      uint8_t scratch[128];
      mspd::Plan p;
      TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(p.open(src, scratch, sizeof(scratch))));
      TEST_ASSERT_TRUE(p.stopsSoftware());
    }
    if (file == "msmf-comp-unknown-kind.bin") {
      cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
      uint8_t scratch[128];
      msmf::Manifest m;
      TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(m.open(src, scratch, sizeof(scratch))));
      TEST_ASSERT_EQUAL_STRING("tags-0000002a.bin", m.tagsName());
      TEST_ASSERT_EQUAL_STRING("autodj-00000029.bin", m.autoDjName());
    }
    // The software's guard reads a newer major on the magic and major alone.
    if (e["why"].str() == "major") TEST_ASSERT_TRUE(cc::unknownMajor(b.data(), b.size()));
  }
}

void test_truncation_at_every_byte() {
  for (const char* name : kGoldenBinaries) {
    const Bytes g = golden(name);
    const std::string format = formatOf(g);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(readAs(format, g)), name);
    for (size_t len = 0; len < g.size(); ++len) {
      Bytes cut(g.begin(), g.begin() + static_cast<long>(len));
      TEST_ASSERT_TRUE_MESSAGE(readAs(format, cut) != cc::Why::Ok, name);
      // And with fileBytes and the header's CRC made to agree: the sections
      // run past the end.
      if (len >= 40) {
        cc::put32(&cut[16], static_cast<uint32_t>(len));
        reseal(cut);
        TEST_ASSERT_TRUE_MESSAGE(readAs(format, cut) != cc::Why::Ok, name);
      }
    }
    // One byte more is no better.
    Bytes longer = g;
    longer.push_back(0);
    TEST_ASSERT_EQUAL_STRING("fileBytes", cc::whyName(readAs(format, longer)));
  }
}

void test_every_flipped_bit() {
  for (const char* name : kGoldenBinaries) {
    const Bytes g = golden(name);
    const std::string format = formatOf(g);
    for (bool device : {false, true}) {
      if (device && format != "MPTG") continue;
      const std::vector<bool> skip = unchecked(g, device);
      Bytes b = g;
      for (size_t i = 0; i < b.size(); ++i)
        for (int bit = 0; bit < 8; ++bit) {
          b[i] ^= static_cast<uint8_t>(1u << bit);
          const cc::Why w = readAs(format, b, device);
          b[i] ^= static_cast<uint8_t>(1u << bit);
          if (skip[i]) {
            TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(w), name);
          } else if (w == cc::Why::Ok) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "%s byte %u bit %d still reads", name, static_cast<unsigned>(i), bit);
            TEST_FAIL_MESSAGE(msg);
          }
        }
    }
  }
}

void test_fuzz_with_resealed_crcs() {
  // Random edits of a golden file, its CRCs made right again, so the
  // structural checks (not the CRCs) meet them: whatever comes out, no
  // crash, no endless loop, and a file that still reads is walkable and
  // its random access stays in bounds.
  std::mt19937 rng(20261007);
  int stillOk = 0, absent = 0;
  for (const char* name : kGoldenBinaries) {
    const Bytes g = golden(name);
    const std::string format = formatOf(g);
    const uint32_t hb = cc::get32(&g[8]);
    for (int iter = 0; iter < 1500; ++iter) {
      Bytes b = g;
      const int edits = 1 + static_cast<int>(rng() % 4);
      for (int k = 0; k < edits; ++k) {
        // Mostly the sections, sometimes the type header or the directory.
        const size_t lo = rng() % 4 == 0 ? 40 : hb;
        const size_t at = lo + rng() % (b.size() - lo);
        switch (rng() % 4) {
          case 0: b[at] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
          case 1: b[at] = static_cast<uint8_t>(rng()); break;
          case 2: b[at] = 0; break;
          default: b[at] = 0xFF; break;
        }
      }
      reseal(b);
      const cc::Why w = readAs(format, b);
      if (w != cc::Why::Ok) {
        ++absent;
        continue;
      }
      ++stillOk;
      if (format == "MPTG") {
        cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
        mptg::File f;
        TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(f.open(src)));
        for (uint32_t r = 0; r < f.info().recordCount; ++r) {
          char path[300];
          size_t len = 0;
          if (f.recordPath(r, path, sizeof(path), &len)) TEST_ASSERT_TRUE(f.find(path, len) != mptg::File::kNotFound);
          mptg::Record rec;
          cc::RunFields run;
          if (f.record(r, &rec)) f.run(rec.strings, &run);
        }
      }
    }
  }
  TEST_ASSERT_TRUE(absent > 0);
  TEST_ASSERT_TRUE(stillOk > 0);  // edits inside tag text are harmless
}

// ---------------------------------------------------------------------------
// The walker on its own terms.
// ---------------------------------------------------------------------------
void test_walker_with_small_buffers_and_a_big_library() {
  // About 3,000 records in a tree up to 7 deep, names of every length a
  // path allows and multi-byte UTF-8, written in random order, walked
  // through the smallest scratch: the same as through a large one, in
  // canonical order, every HIDX lookup back to its record.
  std::mt19937 rng(42);
  const char* syllables[] = {"a", "B", "c d", "\xC3\xA9", "x-", "Zz", "0", "\xE6\x97\xA5", " ", "..q", "_"};
  auto name = [&](size_t maxLen) {
    std::string s;
    const size_t want = 1 + rng() % 24;
    while (s.size() < want) s += syllables[rng() % (sizeof(syllables) / sizeof(*syllables))];
    if (s.size() > maxLen) s = s.substr(0, cc::utf8CutLength(s.data(), s.size(), maxLen));
    if (s == "." || s == ".." || s.empty() || s.back() == ' ') s += "n";
    return s;
  };
  std::vector<std::string> folders = {""};
  for (int i = 0; i < 400; ++i) {
    const std::string& parent = folders[rng() % folders.size()];
    if (std::count(parent.begin(), parent.end(), '/') >= 6) continue;
    const std::string f = parent.empty() ? name(40) : parent + "/" + name(40);
    if (f.size() < 180) folders.push_back(f);
  }
  std::map<std::string, size_t> unique;
  std::vector<std::string> paths;
  while (paths.size() < 3000) {
    const std::string& dir = folders[rng() % folders.size()];
    std::string p = (dir.empty() ? "" : dir + "/") + name(60) + ".mp3";
    if (p.size() > cc::kMaxRelPath) continue;
    if (std::find(folders.begin(), folders.end(), p) != folders.end()) continue;
    if (unique.emplace(p, paths.size()).second) paths.push_back(p);
  }
  std::vector<std::string> titles(paths.size());
  std::vector<mptg::RecordIn> recs(paths.size());
  for (size_t i = 0; i < paths.size(); ++i) {
    titles[i] = "T" + std::to_string(i);
    recs[i].path = paths[i].c_str();
    recs[i].rec.size = static_cast<uint32_t>(i);
    recs[i].rec.track = static_cast<uint16_t>(i % 30);
    recs[i].fields[cc::kTitle] = titles[i].c_str();
    if (i % 3 == 0) recs[i].fields[cc::kGenre] = "Rock\x1FPop";
  }
  mptg::Meta meta;
  meta.source = mptg::kSourceDevice;
  meta.producer = "test";
  VecSink out;
  const char* error = nullptr;
  TEST_ASSERT_TRUE_MESSAGE(mptg::write(out, meta, recs.data(), recs.size(), nullptr, 0, nullptr, 0, nullptr, &error),
                           error ? error : "");
  std::vector<Event> small, large;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(walk(out.b, mptg::kUseAll, &small, mptg::Walker::kMinScratch)));
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(walk(out.b, mptg::kUseAll, &large, 64 * 1024)));
  assertSameWalk(large, small);
  size_t records = 0;
  const Event* prev = nullptr;
  for (const Event& e : small) {
    if (e.folder) continue;
    ++records;
    if (prev) TEST_ASSERT_TRUE(cc::compareFilePaths(prev->path.c_str(), e.path.c_str()) < 0);
    prev = &e;
    const size_t i = unique.at(e.path);
    TEST_ASSERT_EQUAL_UINT32(i, e.r.size);
    TEST_ASSERT_EQUAL_STRING(titles[i].c_str(), e.fields[cc::kTitle].c_str());
  }
  TEST_ASSERT_EQUAL_size_t(paths.size(), records);
  cc::MemSource src(out.b.data(), static_cast<uint32_t>(out.b.size()));
  mptg::File f;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(f.open(src)));
  for (size_t i = 0; i < paths.size(); i += 7) {
    const uint32_t r = f.find(paths[i].data(), paths[i].size());
    TEST_ASSERT_TRUE(r != mptg::File::kNotFound);
    mptg::Record rec;
    TEST_ASSERT_TRUE(f.record(r, &rec));
    TEST_ASSERT_EQUAL_UINT32(i, rec.size);
  }
}

void test_walker_cuts_a_long_server_path_at_a_code_point() {
  // OSTR holds the server's paths, which the card's 255 bytes don't bind:
  // the walker's copy is cut on a code point, File reads it whole.
  std::string server = "Music/";
  while (server.size() < 300) server += "\xC3\xA9";
  mptg::LedgerIn l;
  l.serverPath = server.c_str();
  mptg::RecordIn r;
  r.path = "A/x.mp3";
  r.ledger = &l;
  mptg::Meta meta;
  meta.producer = "a producer whose name runs on past the sixty-three bytes the walker keeps of it";
  VecSink out;
  TEST_ASSERT_TRUE(mptg::write(out, meta, &r, 1, nullptr, 0, nullptr, 0, nullptr, nullptr));
  cc::MemSource src(out.b.data(), static_cast<uint32_t>(out.b.size()));
  std::vector<uint8_t> scratch(1024);
  mptg::Walker w;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(w.begin(src, mptg::kUseAll, scratch.data(), 1024)));
  TEST_ASSERT_EQUAL_size_t(63, std::strlen(w.producer()));
  while (w.next() == mptg::Walker::Step::Folder) {
  }
  TEST_ASSERT_TRUE(w.hasLedger());
  const size_t n = std::strlen(w.serverPath());
  TEST_ASSERT_TRUE(n <= cc::kMaxRelPath && n >= cc::kMaxRelPath - 1);
  TEST_ASSERT_EQUAL_size_t(n, cc::utf8CutLength(w.serverPath(), n, n));  // ends on a whole code point
  TEST_ASSERT_EQUAL_MEMORY(server.data(), w.serverPath(), n);
  mptg::File f;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(f.open(src)));
  mptg::LedgerRow row;
  TEST_ASSERT_TRUE(f.ledger(0, &row));
  char whole[400];
  size_t len = 0;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(f.serverPath(row.serverPath, whole, sizeof(whole), &len)));
  TEST_ASSERT_EQUAL_STRING(server.c_str(), whole);
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(mptg::check(src, mptg::kUseAll, scratch.data(), 1024)));
}

void test_writer_refuses_what_breaks_the_rules() {
  mptg::Meta meta;
  meta.source = mptg::kSourceTransfer;
  auto refuses = [&](std::vector<mptg::RecordIn> recs, std::vector<mptg::FolderIn> folders, const char* what) {
    VecSink out;
    const char* error = nullptr;
    const bool ok = mptg::write(out, meta, recs.data(), recs.size(), folders.data(), folders.size(), nullptr, 0,
                                nullptr, &error);
    TEST_ASSERT_FALSE_MESSAGE(ok, what);
    TEST_ASSERT_NOT_NULL(error);
  };
  mptg::RecordIn a, b;
  a.path = "A/x.mp3";
  b.path = "A/x.mp3";
  refuses({a, b}, {}, "a duplicate path");
  b.path = "A/./y.mp3";
  refuses({b}, {}, "a '.' component");
  b.path = "/A/y.mp3";
  refuses({b}, {}, "an absolute path");
  b.path = "A";
  refuses({a, b}, {}, "a path both a file and a folder");
  const std::string tooLong(cc::kMaxRelPath + 1, 'x');
  b.path = tooLong.c_str();
  refuses({b}, {}, "a path past 255 bytes with /music/");
  b.path = "B/y.mp3";
  b.fields[cc::kTitle] = "a\tb";
  refuses({b}, {}, "a control character");
  b.fields[cc::kTitle] = "a\x1F" "b";
  refuses({b}, {}, "a separator in a single field");
  b.fields[cc::kTitle] = nullptr;
  b.fields[cc::kArtist] = "x\x1Fx";
  refuses({b}, {}, "a repeat in a list");
  b.fields[cc::kArtist] = "x\x1F";
  refuses({b}, {}, "an empty value");
  b.fields[cc::kArtist] = nullptr;
  mptg::FolderIn thumbOnly;
  thumbOnly.path = "Elsewhere";
  thumbOnly.flags = mptg::kFolderThumb;
  refuses({a}, {thumbOnly}, "a folder neither OWNED nor an ancestor");
  meta.source = 0;
  refuses({a}, {}, "source 0");
  meta.source = mptg::kSourceDevice;
  mptg::LedgerIn l;
  a.ledger = &l;
  refuses({a}, {}, "a ledger in a source 1 file");
  // The other writers.
  VecSink out;
  msmf::CompIn dj;
  dj.kind = cc::kMagicMpdj;
  msmf::ManifestIn mi;
  mi.comps = &dj;
  mi.compCount = 1;
  TEST_ASSERT_FALSE(msmf::write(out, mi, nullptr, nullptr, nullptr));
  mspd::OpIn op;
  op.op = 4;
  op.path = "x";
  mspd::PlanIn pi;
  pi.ops = &op;
  pi.opCount = 1;
  TEST_ASSERT_FALSE(mspd::write(out, pi, nullptr, nullptr, nullptr));
  op.op = mspd::kOpDelete;
  op.path = "../x";
  TEST_ASSERT_FALSE(mspd::write(out, pi, nullptr, nullptr, nullptr));
  mpdj::TableIn ti;
  ti.k = 0;
  uint8_t sig[16];
  TEST_ASSERT_FALSE(mpdj::write(out, ti, sig, nullptr, nullptr, nullptr));
}

void test_container_frame() {
  // A file of no sections; then an unknown optional section skipped, a
  // known one of a newer minor's longer stride, a blob with a count.
  VecSink out;
  cc::FileMeta fm;
  fm.magic = cc::kMagicMspd;
  cc::ContainerWriter w;
  const uint8_t th[16] = {};
  cc::SectionOut s[3];
  TEST_ASSERT_TRUE(w.begin(out, fm, th, 56, s, 0));
  uint32_t bytes = 0, crc = 0;
  TEST_ASSERT_TRUE(w.finish(&bytes, &crc));
  TEST_ASSERT_EQUAL_UINT32(56, bytes);
  TEST_ASSERT_EQUAL_size_t(56, out.b.size());
  cc::MemSource src(out.b.data(), static_cast<uint32_t>(out.b.size()));
  cc::Container c;
  TEST_ASSERT_EQUAL_STRING("missing", cc::whyName(c.open(src, mspd::kSpec, nullptr)));  // PEND is required
  // PEND and STRS, an unknown section between them.
  VecSink out2;
  s[0] = cc::SectionOut{mspd::kPend, cc::kSectionRequired, 0, 16, 0};
  s[1] = cc::SectionOut{cc::fourcc("ABCD"), 0, 0, 0, 5};
  s[2] = cc::SectionOut{cc::kStrs, cc::kSectionRequired, 0, 0, 1};
  cc::ContainerWriter w2;
  TEST_ASSERT_TRUE(w2.begin(out2, fm, th, 56, s, 3));
  TEST_ASSERT_TRUE(w2.write("hello", 5));
  TEST_ASSERT_FALSE(w2.finish());  // STRS not written yet
  const uint8_t nul = 0;
  cc::ContainerWriter w3;
  VecSink out3;
  TEST_ASSERT_TRUE(w3.begin(out3, fm, th, 56, s, 3));
  TEST_ASSERT_TRUE(w3.write("hello", 5));
  TEST_ASSERT_TRUE(w3.write(&nul, 1));
  TEST_ASSERT_FALSE(w3.write(&nul, 1));  // past the last section
  cc::ContainerWriter w4;
  VecSink out4;
  TEST_ASSERT_TRUE(w4.begin(out4, fm, th, 56, s, 3));
  TEST_ASSERT_TRUE(w4.write("hello", 5));
  TEST_ASSERT_TRUE(w4.write(&nul, 1));
  TEST_ASSERT_TRUE(w4.finish());
  cc::MemSource src4(out4.b.data(), static_cast<uint32_t>(out4.b.size()));
  uint8_t scratch[64];
  mspd::Plan p;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(p.open(src4, scratch, sizeof(scratch))));
  TEST_ASSERT_EQUAL_UINT32(0, p.opCount());
  // The gap between the unknown section (5 bytes) and STRS is zero padding.
  TEST_ASSERT_EQUAL_UINT32(0, out4.b[56 + 96 + 5]);
  // A header too short, or not a multiple of 8, is refused by the writer.
  cc::ContainerWriter bad;
  TEST_ASSERT_FALSE(bad.begin(out, fm, th, 52, s, 0));
  TEST_ASSERT_FALSE(bad.begin(out, fm, th, 32, s, 0));
  // The stream reads what it was given, and no further.
  const uint8_t data[10] = {'a', 'b', 0, 'c', 0, 'd', 'e', 'f', 'g', 'h'};
  cc::MemSource ds(data, sizeof(data));
  uint8_t buf[3];
  cc::Stream st;
  st.begin(&ds, 0, 8, buf, sizeof(buf));
  char str[8];
  size_t len = 0;
  TEST_ASSERT_TRUE(st.readString(str, sizeof(str), &len));
  TEST_ASSERT_EQUAL_STRING("ab", str);
  TEST_ASSERT_TRUE(st.readString(str, 2, &len));  // cut copy, whole length
  TEST_ASSERT_EQUAL_STRING("c", str);
  TEST_ASSERT_EQUAL_size_t(1, len);
  TEST_ASSERT_FALSE(st.readString(str, sizeof(str), &len));  // "def" has no NUL before the end
  TEST_ASSERT_TRUE(st.failed());
  st.begin(&ds, 0, 10, buf, sizeof(buf));
  TEST_ASSERT_TRUE(st.skipTo(5));
  TEST_ASSERT_FALSE(st.skipTo(4));  // backwards
  st.begin(&ds, 0, 10, buf, sizeof(buf));
  TEST_ASSERT_TRUE(st.drain());
  TEST_ASSERT_EQUAL_HEX32(cc::crc32(data, sizeof(data)), st.crc());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_golden_tags_files);
  RUN_TEST(test_golden_tags_writer_is_order_free);
  RUN_TEST(test_golden_autodj_files);
  RUN_TEST(test_golden_manifests);
  RUN_TEST(test_golden_plan);
  RUN_TEST(test_golden_thumbnail);
  RUN_TEST(test_tags_read_back);
  RUN_TEST(test_tags_enums_read_as_the_table_says);
  RUN_TEST(test_tags_device_and_empty);
  RUN_TEST(test_manifest_read_back);
  RUN_TEST(test_autodj_read_back);
  RUN_TEST(test_plan_read_back);
  RUN_TEST(test_hardening_files);
  RUN_TEST(test_truncation_at_every_byte);
  RUN_TEST(test_every_flipped_bit);
  RUN_TEST(test_fuzz_with_resealed_crcs);
  RUN_TEST(test_walker_with_small_buffers_and_a_big_library);
  RUN_TEST(test_walker_cuts_a_long_server_path_at_a_code_point);
  RUN_TEST(test_writer_refuses_what_breaks_the_rules);
  RUN_TEST(test_container_frame);
  return UNITY_END();
}
