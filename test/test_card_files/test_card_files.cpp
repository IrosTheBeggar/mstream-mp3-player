// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the card contract's binary files (lib/core/
// CardContainer, CardTags, CardManifest, CardAutoDj; docs/METADATA.md 2.4-
// 2.6, 2.12.5, 2.17; docs/AUTODJ-INDEX.md): the C++ writers make the shared
// golden files (test/fixtures/card/golden, made by tools/card_fixtures.py
// and tools/mpdj.py, a second implementation) byte for byte from the
// library descriptions; the readers read them back field for field; every
// hardening file (each check of 2.4.3 and AUTODJ-INDEX.md 8 broken under
// valid CRCs) reads as absent for the reason it breaks, and the newer minor
// as present; truncation at every byte, every flipped bit and a
// resealed-CRC fuzz pass make every file absent (or, for padding and
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
#include <set>
#include <string>
#include <vector>

#include "../support/CardFixtures.h"
#include "CardAutoDj.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "CardManifest.h"
#include "CardTags.h"
#include "NameKey.h"
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
    return w != cc::Why::Ok ? w : t.check(deviceUses ? mpdj::Table::kUseDevice : mpdj::Table::kUseAll, scratch,
                                          sizeof(scratch));
  }
  return cc::Why::Magic;
}

std::string formatOf(const Bytes& b) {
  if (b.size() < 4) return "";
  return std::string(reinterpret_cast<const char*>(b.data()), 4);
}

// MSMF's SRVS (docs/METADATA.md 2.5.5): the software's list of the card's
// other servers, which the player never reads.
constexpr uint32_t kSrvs = cc::fourcc("SRVS");

// The byte ranges a reader doesn't check: the gaps between sections (zero
// padding, which readers ignore), MSMF's SRVS (skipped as any optional
// section the player doesn't know, its CRC unchecked: 2.5.5), and the
// sections a device-uses reader skips (MPTG's HIDX, ORIG and OSTR; MPDJ's
// DJSL, whose slots the device checks as it reads them).
std::vector<bool> unchecked(const Bytes& b, bool deviceUses) {
  std::vector<bool> skip(b.size(), false);
  const uint32_t hb = cc::get32(&b[8]), n = cc::get32(&b[12]);
  uint32_t end = hb + 32 * n;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = &b[hb + 32 * i];
    const uint32_t type = cc::get32(e), off = cc::get32(e + 8), len = cc::get32(e + 12);
    for (uint32_t k = end; k < off; ++k) skip[k] = true;
    if (type == kSrvs ||
        (deviceUses && (type == mptg::kHidx || type == mptg::kOrig || type == mptg::kOstr || type == mpdj::kDjsl)))
      for (uint32_t k = off; k < off + len; ++k) skip[k] = true;
    end = off + len;
  }
  return skip;
}

// A source whose reads fail on demand (the card gone mid-session).
class FlakySource : public cc::Source {
public:
  explicit FlakySource(const Bytes& b) : m_(b.data(), static_cast<uint32_t>(b.size())) {}
  uint32_t size() const override { return m_.size(); }
  bool read(uint32_t offset, void* out, uint32_t n) override { return !fail && m_.read(offset, out, n); }
  bool fail = false;

private:
  cc::MemSource m_;
};

// An MPDJ file's slot CRCs made right again (when its rowCount, slotBytes
// and DJSL's place make sense), so a mutation inside DJSL meets the slot's
// list checks (S2-S4), not its CRC (S1). Before reseal(): DJSL's own CRC
// covers the slots.
void resealSlots(Bytes& b) {
  if (b.size() < mpdj::kMinHeaderBytes || std::memcmp(b.data(), "MPDJ", 4) != 0) return;
  const uint32_t hb = cc::get32(&b[8]), n = cc::get32(&b[12]);
  const uint32_t rows = cc::get32(&b[40]), sb = cc::get16(&b[50]);
  if (n > 64 || static_cast<uint64_t>(hb) + 32ull * n > b.size() || (sb != mpdj::kSmallSlot && sb != mpdj::kLargeSlot))
    return;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = &b[hb + 32 * i];
    if (cc::get32(e) != mpdj::kDjsl) continue;
    const uint64_t off = cc::get32(e + 8);
    if (off + static_cast<uint64_t>(rows) * sb > b.size()) return;
    for (uint32_t r = 0; r < rows; ++r) {
      uint8_t* slot = &b[off + static_cast<uint64_t>(r) * sb];
      cc::put32(slot + sb - 4, mpdj::slotCrc(slot, sb, r));
    }
  }
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

// A file without its last section (of at least two): the directory one
// entry shorter, so every offset 32 bytes lower (the padding unchanged),
// the file ending where the section before it ends, its CRCs made right.
Bytes withoutLastSection(const Bytes& b) {
  const uint32_t hb = cc::get32(&b[8]), n = cc::get32(&b[12]);
  const uint8_t* prev = &b[hb + 32 * (n - 2)];
  const uint32_t end = cc::get32(prev + 8) + cc::get32(prev + 12);
  Bytes out(b.begin(), b.begin() + hb);
  for (uint32_t i = 0; i + 1 < n; ++i) {
    Bytes e(b.begin() + hb + 32 * i, b.begin() + hb + 32 * (i + 1));
    cc::put32(&e[8], cc::get32(&e[8]) - 32);
    out.insert(out.end(), e.begin(), e.end());
  }
  out.insert(out.end(), b.begin() + hb + 32 * n, b.begin() + end);
  cc::put32(&out[12], n - 1);
  cc::put32(&out[16], static_cast<uint32_t>(out.size()));
  reseal(out);
  return out;
}

// An MPDJ description (docs/AUTODJ-INDEX.md 13.2) as the writer's input:
// each row's record values are its primary file's (the smallest path hash,
// a tie by the path's bytes), its keys computed here with namekey and
// checked against the ones the description gives, its embedding the
// description's or its row of the vectors file the description names.
std::string nameKeyOf(const std::string& s) {
  std::string out(s.size() * 3 / 2 + 8, '\0');
  const size_t n = namekey::nameKey(s.data(), s.size(), &out[0], out.size());
  out.resize(n);
  return out;
}

struct AutoDjInput {
  Value d;
  mpdj::TableIn in;
  std::vector<mpdj::RowIn> rows;  // the description's order
  std::vector<std::vector<uint64_t>> paths;  // the primary file's first
  std::vector<std::vector<float>> emb;
  std::vector<std::string> songs;
  std::vector<size_t> order;  // row r (canonical-hash order) is rows[order[r]]

  explicit AutoDjInput(const Value& desc) : d(desc) {
    const Value& rs = d["rows"];
    const size_t n = rs.size();
    const uint32_t dim = static_cast<uint32_t>(d["dim"].u64());
    rows.resize(n);
    paths.resize(n);
    emb.resize(n);
    songs.resize(n);
    std::string side;
    if (d.has("vectors")) {
      side = cardfixtures::bytes("libraries/" + d["vectors"].str());
      TEST_ASSERT_EQUAL_size_t_MESSAGE(4ull * dim * n, side.size(), d["vectors"].c_str());
    }
    for (size_t i = 0; i < n; ++i) {
      const Value& r = rs[i];
      mpdj::RowIn& x = rows[i];
      const Bytes h = hexBytes(r["hash"].str());
      TEST_ASSERT_EQUAL_size_t(16, h.size());
      std::memcpy(x.hash, h.data(), 16);
      for (uint32_t k = 0; k < dim; ++k) {
        if (side.empty()) {
          emb[i].push_back(static_cast<float>(r["embedding"][static_cast<size_t>(k)].num()));
        } else {
          const uint32_t u = cc::get32(reinterpret_cast<const uint8_t*>(side.data()) + 4 * (i * dim + k));
          float f;
          std::memcpy(&f, &u, 4);
          emb[i].push_back(f);
        }
      }
      const Value* primary = nullptr;
      for (size_t f = 0; f < r["files"].size(); ++f) {
        const Value& file = r["files"][f];
        const uint64_t ph = cc::pathHash(file["path"].c_str());
        if (!primary || ph < paths[i][0] || (ph == paths[i][0] && file["path"].str() < (*primary)["path"].str())) {
          primary = &file;
          paths[i].insert(paths[i].begin(), ph);
        } else {
          paths[i].push_back(ph);
        }
      }
      TEST_ASSERT_NOT_NULL(primary);
      if (r.has("primaryPath")) TEST_ASSERT_EQUAL_STRING(r["primaryPath"].c_str(), (*primary)["path"].c_str());
      const Value& p = *primary;
      x.bpm10 = static_cast<uint16_t>(p["bpm10"].u64());
      x.camelot = static_cast<uint8_t>(p["camelot"].u64());
      x.flags = p["bpmAnalysed"].boolean() ? mpdj::kFilterBpmAnalysed : 0;
      // The keys (2.4): the record's artist list, its values joined by U+001F.
      std::string list, first;
      for (size_t a = 0; a < p["artists"].size(); ++a) {
        const std::string& v = p["artists"][a].str();
        if (a) list += '\x1F';
        list += v;
        if (first.empty() && !namekey::blank(v.data(), v.size())) first = nameKeyOf(v);
      }
      x.artistKey = namekey::artistKey(list.data(), list.size());
      TEST_ASSERT_EQUAL_UINT32(first.empty() ? 0 : static_cast<uint32_t>(cc::fnv1a64Str(first.c_str())), x.artistKey);
      if (r.has("artistNameKey")) TEST_ASSERT_EQUAL_STRING(r["artistNameKey"].c_str(), first.c_str());
      const std::string& title = p["title"].str();
      if (p.has("title") && !namekey::blank(title.data(), title.size())) {
        char display[namekey::kDisplayMax + 1];
        namekey::displayJoin(list.data(), list.size(), display, sizeof(display));
        songs[i] = nameKeyOf(display) + "|" + nameKeyOf(title);
        x.songKey = songs[i].c_str();
        if (r.has("songKey")) TEST_ASSERT_EQUAL_STRING(r["songKey"].c_str(), x.songKey);
      } else {
        x.songKey = nullptr;
        if (r.has("songKey")) TEST_ASSERT_TRUE_MESSAGE(r["songKey"].isNull(), r["hash"].c_str());
      }
      x.paths = paths[i].data();
      x.pathCount = static_cast<uint32_t>(paths[i].size());
      x.embedding = emb[i].data();
    }
    order.resize(n);
    for (size_t i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return std::memcmp(rows[a].hash, rows[b].hash, 16) < 0; });
    in.generation = static_cast<uint32_t>(d["generation"].u64());
    in.cardId = d["cardId"].u64();
    in.minor = static_cast<uint16_t>(d["minor"].u64());
    in.k = static_cast<uint32_t>(d["k"].u64(mpdj::kDefaultK));
    in.selKind = static_cast<uint8_t>(d["selKind"].u64(mpdj::kSelStrided));
    in.selHead = static_cast<uint32_t>(d["selHead"].u64(mpdj::kDefaultHead));
    in.selFloor = static_cast<uint32_t>(d["selFloor"].u64(mpdj::kDefaultFloor));
    in.builtTime = static_cast<uint32_t>(d["builtTime"].u64());
    in.tagsGeneration = static_cast<uint32_t>(d["tagsGeneration"].u64());
    in.modelId = d["modelId"].c_str();
    in.modelVersion = d["modelVersion"].c_str();
    in.metric = d["metric"].c_str();
    in.license = d["license"].c_str();
    in.attribution = d["attribution"].c_str();
    in.dim = dim;
    in.rows = rows.data();
    in.rowCount = static_cast<uint32_t>(n);
  }
  AutoDjInput(const AutoDjInput&) = delete;
  AutoDjInput& operator=(const AutoDjInput&) = delete;

  const mpdj::RowIn& row(uint32_t r) const { return rows[order[r]]; }
  // Every row's list by 5's rule, as plainly as it reads: every candidate's
  // level, a sort by (level desc, row asc), the ranks.
  std::vector<std::vector<mpdj::Entry>> ruleLists() const {
    const uint32_t n = in.rowCount, k = in.k;
    std::vector<std::vector<mpdj::Entry>> out(n);
    for (uint32_t a = 0; a < n; ++a) {
      std::vector<std::pair<int, uint32_t>> cand;  // (-level, row)
      for (uint32_t b = 0; b < n; ++b) {
        if (b == a) continue;
        if (row(a).songKey && row(b).songKey && std::strcmp(row(a).songKey, row(b).songKey) == 0) continue;
        cand.emplace_back(-static_cast<int>(mpdj::level(row(a).embedding, row(b).embedding, in.dim)), b);
      }
      std::sort(cand.begin(), cand.end());
      uint32_t m = 0;
      while (m < cand.size() && -cand[m].first >= static_cast<int>(in.selFloor)) ++m;
      const bool strided = in.selKind == mpdj::kSelStrided && m > k;
      for (uint32_t j = 0; j < k && j < cand.size(); ++j) {
        const uint32_t h = in.selHead;
        const uint32_t r = !strided || j < h ? j : h + (j - h) * (m - 1 - h) / (k - 1 - h);
        mpdj::Entry e;
        e.row = static_cast<uint16_t>(cand[r].second);
        e.level = static_cast<uint8_t>(-cand[r].first);
        out[a].push_back(e);
      }
    }
    return out;
  }
};

// A file's DJPH entries (hash, row), read as they stand.
std::vector<std::pair<uint64_t, uint32_t>> djphOf(const Bytes& file) {
  cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
  cc::Container c;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(c.open(src, mpdj::kSpec, nullptr)));
  const cc::Section& s = c.section(mpdj::kSecPaths);
  std::vector<std::pair<uint64_t, uint32_t>> out;
  for (uint32_t i = 0; i < s.count; ++i) {
    const uint8_t* e = file.data() + s.offset + i * s.stride;
    out.emplace_back(cc::get64(e), cc::get16(e + 8));
  }
  return out;
}

// Two MPDJ files read the same (the hardening's `same`): every row's filter
// record and slot, every DJPH hash's row, the five strings.
void assertSameAutoDj(const Bytes& want, const Bytes& got, const char* what) {
  cc::MemSource ws(want.data(), static_cast<uint32_t>(want.size())), gs(got.data(), static_cast<uint32_t>(got.size()));
  mpdj::Table w, g;
  TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(w.open(ws)), what);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(g.open(gs)), what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(w.rowCount(), g.rowCount(), what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(w.k(), g.k(), what);
  uint8_t wb[mpdj::kLargeSlot], gb[mpdj::kLargeSlot];
  mpdj::Entry we[mpdj::kMaxK], ge[mpdj::kMaxK];
  for (uint32_t r = 0; r < w.rowCount(); ++r) {
    mpdj::Filter wf, gf;
    TEST_ASSERT_TRUE(w.filter(r, &wf) && g.filter(r, &gf));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(wf.bpm10, gf.bpm10, what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(wf.camelot, gf.camelot, what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(wf.flags, gf.flags, what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(wf.artistKey, gf.artistKey, what);
    uint32_t wc = 0, gc = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(w.slot(r, wb, we, &wc)), static_cast<int>(g.slot(r, gb, ge, &gc)),
                                  what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(wc, gc, what);
    for (uint32_t j = 0; j < wc; ++j) {
      TEST_ASSERT_EQUAL_UINT16_MESSAGE(we[j].row, ge[j].row, what);
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(we[j].level, ge[j].level, what);
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(djphOf(want) == djphOf(got), what);
  for (uint32_t i = 0; i < 5; ++i) {
    char a[300], b[300];
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(w.string(i, a, sizeof(a))));
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(g.string(i, b, sizeof(b))));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a, b, what);
  }
}

const char* const kAutoDjLibraries[] = {"autodj-00000029", "autodj-small-top", "autodj-small-default", "autodj-tiny",
                                        "autodj-pair",     "autodj-mid",       "autodj-mid-default"};

// The goldens the byte loops run over (not autodj-mid's two: too big for
// a check per flipped bit).
const char* const kGoldenBinaries[] = {"tags-0000002a.bin",    "tags-device.bin",          "tags-00000001.bin",
                                       "tags-00000007.bin",    "manifest.bin",             "manifest-min.bin",
                                       "manifest-servers.bin", "pending.bin",              "autodj-00000029.bin",
                                       "autodj-small-top.bin", "autodj-small-default.bin", "autodj-tiny.bin",
                                       "autodj-pair.bin"};

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
  for (const char* name : kAutoDjLibraries) {
    AutoDjInput x(library(name));
    VecSink out;
    uint8_t sig[16];
    uint32_t fileBytes = 0, headerCrc = 0;
    const char* error = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(mpdj::write(out, x.in, sig, &fileBytes, &headerCrc, &error), error ? error : name);
    const Bytes want = golden(x.d["file"].str());
    TEST_ASSERT_TRUE_MESSAGE(!want.empty(), name);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(want.size(), out.b.size(), name);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want.data(), out.b.data(), want.size(), name);
    TEST_ASSERT_EQUAL_UINT32(want.size(), fileBytes);
    TEST_ASSERT_EQUAL_HEX32(cc::get32(&want[32]), headerCrc);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&want[64], sig, 16, name);  // the header's selectionSig
  }
  // The writer takes its rows in any order.
  AutoDjInput x(library("autodj-00000029"));
  std::mt19937 rng(11);
  std::shuffle(x.rows.begin(), x.rows.end(), rng);
  for (auto& p : x.paths) std::reverse(p.begin(), p.end());
  VecSink out;
  uint8_t sig[16];
  TEST_ASSERT_TRUE(mpdj::write(out, x.in, sig, nullptr, nullptr, nullptr));
  TEST_ASSERT_TRUE(out.b == golden("autodj-00000029.bin"));
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

// A root description through the C++ writer (which writes no SRVS: 2.17).
Bytes manifestFrom(const Value& d) {
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
  TEST_ASSERT_TRUE_MESSAGE(msmf::write(out, in, nullptr, nullptr, &error), error ? error : d["file"].c_str());
  return out.b;
}

void test_golden_manifests() {
  for (const char* name : {"manifest", "manifest-min"}) {
    const Value d = library(name);
    const Bytes got = manifestFrom(d);
    const Bytes want = golden(d["file"].str());
    TEST_ASSERT_EQUAL_size_t_MESSAGE(want.size(), got.size(), name);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want.data(), got.data(), want.size(), name);
  }
}

// A card filled from several servers (2.5.5, 2.6.6). The C++ writers
// predate ORIG's server and SRVS (2.17, item 2), so their bytes for the two
// goldens differ exactly there; the player reads both goldens as those
// bytes, since it reads neither.
void test_goldens_of_several_servers() {
  // The tags file: the C++ writer's is the golden with every row's server
  // 0, its CRCs made right.
  const Value d = library("tags-00000007");
  TagsInput in(d);
  VecSink out;
  const char* error = nullptr;
  TEST_ASSERT_TRUE_MESSAGE(in.write(&out, d["producer"].str(), &error), error ? error : "tags-00000007");
  const Bytes tags = golden("tags-00000007.bin");
  cc::MemSource src(tags.data(), static_cast<uint32_t>(tags.size()));
  mptg::File f;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(f.open(src)));
  const cc::Section& orig = f.container().section(mptg::kSecOrig);
  TEST_ASSERT_TRUE(orig.present());
  std::map<std::string, uint32_t> server;  // each record's, as the description gives it
  for (size_t i = 0; i < d["records"].size(); ++i)
    server[d["records"][i]["path"].str()] = static_cast<uint32_t>(d["records"][i]["ledger"]["server"].u64());
  Bytes zeroed = tags;
  std::set<uint32_t> seen;
  for (uint32_t r = 0; r < f.info().recordCount; ++r) {
    char path[300];
    size_t len = 0;
    TEST_ASSERT_TRUE(f.recordPath(r, path, sizeof(path), &len));
    uint8_t* row = &zeroed[orig.offset + r * orig.stride];
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(server[path], cc::get16(row + 74), path);  // bytes 74-75, then 0s
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, cc::get32(row + 76), path);
    seen.insert(cc::get16(row + 74));
    cc::put16(row + 74, 0);
  }
  TEST_ASSERT_EQUAL_size_t(4, seen.size());  // servers 0-3
  reseal(zeroed);
  TEST_ASSERT_TRUE(zeroed == out.b);
  for (uint32_t uses : {0u, mptg::kUseAll}) {
    std::vector<Event> a, b;
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(walk(tags, uses, &a)));
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(walk(out.b, uses, &b)));
    assertSameWalk(b, a);
  }
  // The root: SRVS is its last section, optional, three rows of 40 bytes;
  // without it, the golden is the C++ writer's root of the same
  // description. The player reads it as that root, naming that tags file.
  const Value md = library("manifest-servers");
  const Bytes root = golden("manifest-servers.bin");
  const uint32_t hb = cc::get32(&root[8]), n = cc::get32(&root[12]);
  const uint8_t* e = &root[hb + 32 * (n - 1)];
  TEST_ASSERT_EQUAL_HEX32(kSrvs, cc::get32(e));
  TEST_ASSERT_EQUAL_UINT32(0, cc::get32(e + 4));
  TEST_ASSERT_EQUAL_UINT32(md["servers"].size(), cc::get32(e + 16));
  TEST_ASSERT_EQUAL_UINT32(40, cc::get32(e + 20));
  TEST_ASSERT_TRUE(withoutLastSection(root) == manifestFrom(md));
  cc::MemSource rs(root.data(), static_cast<uint32_t>(root.size()));
  uint8_t scratch[256];
  msmf::Manifest m;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(m.open(rs, scratch, sizeof(scratch))));
  TEST_ASSERT_EQUAL_STRING("tags-00000007.bin", m.tagsName());
  TEST_ASSERT_FALSE(m.hasAutoDj());
  TEST_ASSERT_TRUE(m.serverUrlKey() == 0xED901EA3EE763AC7ull);
  TEST_ASSERT_TRUE(msmf::companionMatches(m.tags(), f.info().frame, f.info().source, nullptr));
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
  // Every golden: its slots are 5's lists, recomputed here as plainly as
  // the rule reads; filters and paths are its primary files'; the bytes a
  // slot doesn't use are zeros.
  using L = std::vector<std::vector<std::pair<int, int>>>;
  const L g1 = {{{2, 200}, {5, 185}, {4, 162}, {3, 100}}, {{0, 100}, {2, 51}, {4, 40}, {3, 0}},
                {{0, 200}, {5, 171}, {4, 150}, {3, 128}}, {{2, 128}, {0, 100}, {1, 0}},
                {{0, 162}, {2, 150}, {1, 40}},            {{0, 185}, {2, 171}, {1, 0}}};
  L g2 = g1, g3 = g1;
  g2[0] = {{2, 200}, {5, 185}, {4, 162}, {1, 100}};
  g3[0] = {{2, 200}, {5, 185}, {4, 162}, {1, 100}, {3, 100}};
  g3[1] = {{0, 100}, {2, 51}, {4, 40}, {3, 0}, {5, 0}};
  g3[2] = {{0, 200}, {5, 171}, {4, 150}, {3, 128}, {1, 51}};
  // AUTODJ-INDEX.md 13.4-13.5's lists, typed.
  const std::map<std::string, L> typed = {{"autodj-00000029", g1},
                                          {"autodj-small-top", g2},
                                          {"autodj-small-default", g3},
                                          {"autodj-tiny", {{}, {}}},
                                          {"autodj-pair", {{{1, 0}}, {{0, 0}}}}};
  uint8_t scratch[1024], buf[mpdj::kLargeSlot];
  mpdj::Entry e[mpdj::kMaxK];
  for (const char* name : kAutoDjLibraries) {
    AutoDjInput x(library(name));
    const Bytes file = golden(x.d["file"].str());
    cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
    mpdj::Table t;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(t.open(src)), name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(t.check(mpdj::Table::kUseAll, scratch, sizeof(scratch))), name);
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.check(mpdj::Table::kUseDevice, scratch, 64)));
    TEST_ASSERT_EQUAL_UINT32(x.in.rowCount, t.rowCount());
    TEST_ASSERT_EQUAL_UINT32(x.in.k, t.k());
    TEST_ASSERT_EQUAL_UINT32(mpdj::slotBytesFor(x.in.k), t.slotBytes());
    TEST_ASSERT_EQUAL_UINT8(x.in.selKind, t.selKind());
    TEST_ASSERT_EQUAL_UINT32(x.in.selKind == mpdj::kSelTop ? x.in.k : x.in.selHead, t.selHead());
    TEST_ASSERT_EQUAL_UINT32(x.in.selKind == mpdj::kSelTop ? 0 : x.in.selFloor, t.selFloor());
    TEST_ASSERT_EQUAL_UINT32(x.in.builtTime, t.builtTime());
    TEST_ASSERT_EQUAL_UINT32(x.in.tagsGeneration, t.tagsGeneration());
    TEST_ASSERT_EQUAL_UINT32(x.in.generation, t.frame().generation);
    TEST_ASSERT_TRUE(t.frame().cardId == x.in.cardId);
    TEST_ASSERT_EQUAL_UINT16(mpdj::kMajor2, t.frame().major);
    // The alignment rule: DJSL right after the directory, on a slot boundary.
    TEST_ASSERT_EQUAL_UINT32(mpdj::headerBytesFor(4, t.slotBytes()), t.frame().headerBytes);
    TEST_ASSERT_EQUAL_UINT32(t.frame().headerBytes + 4 * cc::kDirEntryBytes, t.slotsOffset());
    TEST_ASSERT_EQUAL_UINT32(0, t.slotsOffset() % t.slotBytes());
    const char* const strings[5] = {"modelId", "modelVersion", "metric", "license", "attribution"};
    for (uint32_t i = 0; i < 5; ++i) {
      char s[300];
      TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.string(i, s, sizeof(s))));
      TEST_ASSERT_EQUAL_STRING(x.d[strings[i]].c_str(), s);
    }
    const auto lists = x.ruleLists();
    const auto it = typed.find(name);
    std::set<uint64_t> seen, twice;
    for (uint32_t r = 0; r < t.rowCount(); ++r)
      for (uint32_t p = 0; p < x.row(r).pathCount; ++p)
        if (!seen.insert(x.row(r).paths[p]).second) twice.insert(x.row(r).paths[p]);
    uint32_t paths = 0;
    for (uint32_t r = 0; r < t.rowCount(); ++r) {
      const mpdj::RowIn& want = x.row(r);
      mpdj::Filter f;
      TEST_ASSERT_TRUE(t.filter(r, &f));
      TEST_ASSERT_EQUAL_UINT16(want.bpm10, f.bpm10);
      TEST_ASSERT_EQUAL_UINT8(want.camelot <= 24 ? want.camelot : 0, f.camelot);
      TEST_ASSERT_EQUAL_UINT8(want.flags, f.flags);
      TEST_ASSERT_EQUAL_UINT32(want.artistKey, f.artistKey);
      for (uint32_t p = 0; p < want.pathCount; ++p) {
        const bool kept = !twice.count(want.paths[p]);
        TEST_ASSERT_EQUAL_UINT32(kept ? r : mpdj::kNoRow, t.rowOf(want.paths[p]));
        paths += kept;
      }
      uint32_t c = 0;
      cc::Why why = cc::Why::Io;
      TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Ok), static_cast<int>(t.slot(r, buf, e, &c, &why)));
      TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(why));
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(lists[r].size(), c, name);
      for (uint32_t j = 0; j < c; ++j) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(lists[r][j].row, e[j].row, name);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(lists[r][j].level, e[j].level, name);
      }
      if (it != typed.end()) {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(it->second[r].size(), c, name);
        for (uint32_t j = 0; j < c; ++j) {
          TEST_ASSERT_EQUAL_UINT16_MESSAGE(it->second[r][j].first, e[j].row, name);
          TEST_ASSERT_EQUAL_UINT8_MESSAGE(it->second[r][j].second, e[j].level, name);
        }
      }
      // Unused entries: 0xFFFF and level 0; then zeros to the CRC.
      const uint32_t k = t.k(), sb = t.slotBytes();
      for (uint32_t j = c; j < k; ++j) {
        TEST_ASSERT_EQUAL_HEX16(mpdj::kUnused, cc::get16(buf + 2 * j));
        TEST_ASSERT_EQUAL_UINT8(0, buf[2 * k + j]);
      }
      for (uint32_t b = 3 * k; b < sb - 4; ++b) TEST_ASSERT_EQUAL_UINT8(0, buf[b]);
      TEST_ASSERT_EQUAL_HEX32(mpdj::slotCrc(buf, sb, r), cc::get32(buf + sb - 4));
    }
    TEST_ASSERT_EQUAL_UINT32(paths, t.pathCount());
    TEST_ASSERT_EQUAL_UINT32(mpdj::kNoRow, t.rowOf(12345));
  }

  // autodj-00000029 field by field (13.4).
  const Bytes dj = golden("autodj-00000029.bin");
  TEST_ASSERT_EQUAL_size_t(1973, dj.size());
  cc::MemSource src(dj.data(), static_cast<uint32_t>(dj.size()));
  mpdj::Table t;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
  TEST_ASSERT_EQUAL_UINT32(6, t.rowCount());
  TEST_ASSERT_EQUAL_UINT32(7, t.pathCount());  // one recording at two paths
  TEST_ASSERT_EQUAL_UINT32(4, t.k());
  TEST_ASSERT_EQUAL_UINT32(256, t.slotBytes());
  TEST_ASSERT_EQUAL_UINT8(mpdj::kSelStrided, t.selKind());
  TEST_ASSERT_EQUAL_UINT32(1, t.selHead());
  TEST_ASSERT_EQUAL_UINT32(100, t.selFloor());
  TEST_ASSERT_EQUAL_UINT32(128, t.frame().headerBytes);
  TEST_ASSERT_EQUAL_UINT32(41, t.frame().generation);
  TEST_ASSERT_EQUAL_UINT32(1790000000u, t.builtTime());
  TEST_ASSERT_EQUAL_UINT32(256, t.slotsOffset());
  char s[64];
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.string(0, s, sizeof(s))));
  TEST_ASSERT_EQUAL_STRING("synthetic-embedding", s);
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.string(3, s, sizeof(s))));
  TEST_ASSERT_EQUAL_STRING("CC0-1.0", s);
  // Row 1, one recording at two paths: its NFC file is the primary (the
  // smaller hash), bpm10 0; the NFD file's 1000 isn't the row's.
  mpdj::Filter f;
  TEST_ASSERT_TRUE(t.filter(1, &f));
  TEST_ASSERT_EQUAL_UINT16(0, f.bpm10);
  TEST_ASSERT_EQUAL_UINT32(0, f.artistKey);  // no artist
  const char* nfc = "Generated Artist 02/Sample Album 02/01 - Sample Caf\xC3\xA9.flac";
  const char* nfd = "Generated Artist 02/Sample Album 02/01 - Sample Cafe\xCC\x81.flac";
  TEST_ASSERT_TRUE(cc::pathHash(nfc) < cc::pathHash(nfd));
  TEST_ASSERT_EQUAL_UINT32(1, t.rowOf(cc::pathHash(nfc)));
  TEST_ASSERT_EQUAL_UINT32(1, t.rowOf(cc::pathHash(nfd)));
  // Row 2: three artist values, key 1B, BPM analysed.
  TEST_ASSERT_TRUE(t.filter(2, &f));
  TEST_ASSERT_EQUAL_UINT16(1280, f.bpm10);
  TEST_ASSERT_EQUAL_UINT8(13, f.camelot);
  TEST_ASSERT_EQUAL_UINT8(mpdj::kFilterBpmAnalysed, f.flags);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(cc::fnv1a64Str("generated artist 04")), f.artistKey);
  // The cosine of rows 2 and 3 is exactly 0.5: 127.5, level 128.
  uint8_t slot[256];
  uint32_t c = 0;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Ok), static_cast<int>(t.slot(3, slot, e, &c)));
  TEST_ASSERT_EQUAL_UINT32(3, c);  // rows 4 and 5 are its song: an unused entry
  TEST_ASSERT_EQUAL_UINT16(2, e[0].row);
  TEST_ASSERT_EQUAL_UINT8(128, e[0].level);
  // A slot read for the wrong row: its CRC fails.
  cc::Why why = cc::Why::Ok;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Damaged),
                        static_cast<int>(mpdj::checkSlot(slot, 256, 4, 2, 6, e, &c, &why)));
  TEST_ASSERT_EQUAL_STRING("djSlotCrc", cc::whyName(why));
  // No row 6: no checked row names it, so the caller is wrong (Broken).
  TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Broken), static_cast<int>(t.slot(6, slot, e, &c, &why)));
  TEST_ASSERT_EQUAL_STRING("djSlotIndex", cc::whyName(why));
  // A read that fails is the card's: Damaged, worth one more read.
  {
    FlakySource fs(dj);
    mpdj::Table ft;
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(ft.open(fs)));
    fs.fail = true;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Damaged), static_cast<int>(ft.slot(3, slot, e, &c, &why)));
    TEST_ASSERT_EQUAL_STRING("io", cc::whyName(why));
    fs.fail = false;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Ok), static_cast<int>(ft.slot(3, slot, e, &c, &why)));
  }

  // autodj-small-default (13.5): 512-byte slots, the 1-byte padding at 507.
  const Bytes big = golden("autodj-small-default.bin");
  cc::MemSource bs(big.data(), static_cast<uint32_t>(big.size()));
  mpdj::Table bt;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(bt.open(bs)));
  TEST_ASSERT_EQUAL_UINT32(512, bt.slotBytes());
  TEST_ASSERT_EQUAL_UINT32(384, bt.frame().headerBytes);
  TEST_ASSERT_EQUAL_UINT32(512, bt.slotsOffset());
  TEST_ASSERT_EQUAL_UINT32(mpdj::kDefaultK, bt.k());
  TEST_ASSERT_EQUAL_UINT32(mpdj::kDefaultHead, bt.selHead());
  TEST_ASSERT_EQUAL_UINT32(mpdj::kDefaultFloor, bt.selFloor());

  // autodj-mid (13.5): the float32 trap. The rule's level of rows A and B is
  // 140, the floor; a float32 sum gives 139.
  Value vectors;
  TEST_ASSERT_TRUE(cardfixtures::json("vectors.json", &vectors));
  const Value& tv = vectors["mpdj"]["trap"];
  AutoDjInput mid(library("autodj-mid"));
  TEST_ASSERT_EQUAL_STRING(tv["file"].c_str(), mid.d["file"].c_str());
  const mpdj::RowIn& ra = mid.row(static_cast<uint32_t>(tv["rowA"].u64()));
  const mpdj::RowIn& rb = mid.row(static_cast<uint32_t>(tv["rowB"].u64()));
  TEST_ASSERT_EQUAL_UINT8(tv["level"].u64(), mpdj::level(ra.embedding, rb.embedding, mid.in.dim));
  float s32 = 0;
  for (uint32_t i = 0; i < mid.in.dim; ++i) s32 = s32 + ra.embedding[i] * rb.embedding[i];
  TEST_ASSERT_EQUAL_UINT8(tv["levelF32"].u64(), mpdj::level(static_cast<double>(s32)));
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
    if (e.has("same") && e["format"].str() == "MPDJ") {
      assertSameAutoDj(golden(e["same"].str()), b, what.c_str());
    } else if (e.has("same")) {
      std::vector<Event> a, g;
      TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(walk(b, mptg::kUseAll, &a)));
      TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(walk(golden(e["same"].str()), mptg::kUseAll, &g)));
      assertSameWalk(g, a);
    }
    if (e.has("slot")) {
      // One slot as the device reads it, the join's sections good:
      // damaged (its CRC) or broken (its list, under a good CRC).
      cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
      uint8_t scratch[64], buf[mpdj::kLargeSlot];
      mpdj::Entry entries[mpdj::kMaxK];
      uint32_t count = 0;
      mpdj::Table t;
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(t.open(src)), what.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(t.check(mpdj::Table::kUseDevice, scratch, sizeof(scratch))),
                                       what.c_str());
      const std::string& read = e["slot"]["read"].str();
      const mpdj::SlotRead want = read == "damaged" ? mpdj::SlotRead::Damaged : mpdj::SlotRead::Broken;
      TEST_ASSERT_TRUE_MESSAGE(read == "damaged" || read == "broken", what.c_str());
      cc::Why slotWhy = cc::Why::Ok;
      const mpdj::SlotRead got = t.slot(static_cast<uint32_t>(e["slot"]["row"].u64()), buf, entries, &count, &slotWhy);
      TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(want), static_cast<int>(got), what.c_str());
      TEST_ASSERT_EQUAL_UINT32(0, count);
      if (e["why"].str() != "ok")
        TEST_ASSERT_EQUAL_STRING_MESSAGE(e["why"].c_str(), cc::whyName(slotWhy), what.c_str());
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
    // The software's guard reads a newer major on the magic and major alone
    // (MPDJ's major 1, the retired draft, is an older one it replaces).
    if (e["why"].str() == "major")
      TEST_ASSERT_EQUAL_MESSAGE(file != "mpdj-major-1.bin", cc::unknownMajor(b.data(), b.size()), what.c_str());
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
      if (device && format != "MPTG" && format != "MPDJ") continue;
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
  // Random edits of a golden file, its CRCs made right again (an MPDJ
  // file's slot CRCs too), so the structural checks (not the CRCs) meet
  // them: whatever comes out, no crash, no endless loop, and a file that
  // still reads is walkable and its random access stays in bounds.
  std::mt19937 rng(20261007);
  int stillOk = 0, absent = 0, slotLists = 0;
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
      resealSlots(b);
      reseal(b);
      const cc::Why w = readAs(format, b);
      if (w != cc::Why::Ok) {
        ++absent;
        slotLists += w == cc::Why::DjSlotUnused || w == cc::Why::DjSlotIndex || w == cc::Why::DjSlotOrder;
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
      } else if (format == "MPDJ") {
        // Every filter and slot reads; every path finds its row.
        cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
        mpdj::Table t;
        TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(t.open(src)));
        uint8_t buf[mpdj::kLargeSlot];
        mpdj::Entry entries[mpdj::kMaxK];
        for (uint32_t r = 0; r < t.rowCount(); ++r) {
          mpdj::Filter f;
          TEST_ASSERT_TRUE(t.filter(r, &f));
          uint32_t c = 0;
          TEST_ASSERT_EQUAL_INT(static_cast<int>(mpdj::SlotRead::Ok), static_cast<int>(t.slot(r, buf, entries, &c)));
          for (uint32_t j = 0; j < c; ++j) TEST_ASSERT_TRUE(entries[j].row < t.rowCount());
        }
        for (const auto& ph : djphOf(b)) TEST_ASSERT_EQUAL_UINT32(ph.second, t.rowOf(ph.first));
      }
    }
  }
  TEST_ASSERT_TRUE(absent > 0);
  TEST_ASSERT_TRUE(stillOk > 0);  // edits inside tag text are harmless
  // The slots' CRCs made right, edits in DJSL reach the lists' own checks.
  TEST_ASSERT_TRUE(slotLists > 0);
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
  // MPDJ (docs/AUTODJ-INDEX.md 4, 5): one valid table, then each rule
  // broken in turn.
  const uint64_t pa = cc::pathHash("A/x.mp3"), pb = cc::pathHash("B/y.mp3");
  const float ea[2] = {1, 0}, eb[2] = {0.6f, 0.8f};
  mpdj::RowIn ra, rb;
  ra.hash[0] = 1;
  ra.paths = &pa;
  ra.pathCount = 1;
  ra.embedding = ea;
  rb.hash[0] = 2;
  rb.paths = &pb;
  rb.pathCount = 1;
  rb.embedding = eb;
  mpdj::RowIn rows[2] = {ra, rb};
  mpdj::TableIn good;
  good.k = 4;
  good.selHead = 1;
  good.selFloor = 100;
  good.dim = 2;
  good.rows = rows;
  good.rowCount = 2;
  uint8_t sig[16];
  const char* error = nullptr;
  TEST_ASSERT_TRUE(mpdj::write(out, good, sig, nullptr, nullptr, &error));
  auto djRefuses = [&](mpdj::TableIn ti, const char* what) {
    VecSink o;
    const char* why = nullptr;
    TEST_ASSERT_FALSE_MESSAGE(mpdj::write(o, ti, sig, nullptr, nullptr, &why), what);
    TEST_ASSERT_NOT_NULL(why);
  };
  mpdj::TableIn ti = good;
  ti.k = 1;
  djRefuses(ti, "k 1");
  ti.k = 170;
  djRefuses(ti, "k 170");
  ti = good;
  ti.selHead = 0;
  djRefuses(ti, "selKind 1, selHead 0");
  ti.selHead = 3;
  djRefuses(ti, "selKind 1, selHead k - 1");
  ti = good;
  ti.k = 2;  // selKind 1 needs k >= 3 (AUTODJ-INDEX.md 4.1)
  djRefuses(ti, "selKind 1 at k 2");
  ti = good;
  ti.k = 50;
  ti.selHead = mpdj::kDefaultHead;  // 84 doesn't fit k 50
  djRefuses(ti, "selKind 1, the default head at k 50");
  ti = good;
  ti.selFloor = 0;
  djRefuses(ti, "selKind 1, selFloor 0");
  ti.selFloor = 256;
  djRefuses(ti, "selKind 1, selFloor 256");
  ti = good;
  ti.selKind = 2;
  djRefuses(ti, "selKind 2");
  ti = good;
  ti.rowCount = 0;
  djRefuses(ti, "no row");
  ti = good;
  ti.dim = 0;
  djRefuses(ti, "dim 0");
  const std::string longest(mpdj::kMaxString, 'x'), overLong(mpdj::kMaxString + 1, 'x');
  ti = good;
  ti.attribution = longest.c_str();
  TEST_ASSERT_TRUE(mpdj::write(out, ti, sig, nullptr, nullptr, nullptr));
  ti.attribution = overLong.c_str();
  djRefuses(ti, "a string over 255 bytes");
  mpdj::RowIn twice[2] = {ra, ra};
  ti = good;
  ti.rows = twice;
  djRefuses(ti, "a hash twice");
  mpdj::RowIn noPath[2] = {ra, rb};
  noPath[1].pathCount = 0;
  ti.rows = noPath;
  djRefuses(ti, "a row with no path");
  mpdj::RowIn noVector[2] = {ra, rb};
  noVector[1].embedding = nullptr;
  ti.rows = noVector;
  djRefuses(ti, "a row with no embedding");
  const float notANumber[2] = {std::nanf(""), 0};
  mpdj::RowIn notFinite[2] = {ra, rb};
  notFinite[1].embedding = notANumber;
  ti.rows = notFinite;
  djRefuses(ti, "a NaN");
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
  // A type header shorter than headerBytes - 40 (MPDJ's 64 bytes in a
  // header of up to 384): zeros up to headerBytes, under the header's CRC;
  // the same file as a whole type header of those zeros.
  uint8_t typed[8] = {1, 2, 3, 4, 5, 6, 7, 8}, whole[88] = {1, 2, 3, 4, 5, 6, 7, 8};
  VecSink padded, full;
  cc::ContainerWriter wp, wf;
  TEST_ASSERT_TRUE(wp.begin(padded, fm, typed, 128, s, 0, sizeof(typed)));
  TEST_ASSERT_TRUE(wp.finish());
  TEST_ASSERT_TRUE(wf.begin(full, fm, whole, 128, s, 0));
  TEST_ASSERT_TRUE(wf.finish());
  TEST_ASSERT_EQUAL_size_t(128, padded.b.size());
  TEST_ASSERT_TRUE(padded.b == full.b);
  TEST_ASSERT_FALSE(bad.begin(out, fm, typed, 128, s, 0, 89));   // past headerBytes
  TEST_ASSERT_FALSE(bad.begin(out, fm, whole, 304, s, 0, 257));  // past the writer's 256
  TEST_ASSERT_TRUE(bad.begin(out, fm, whole, 400, s, 0, 64));
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
  RUN_TEST(test_goldens_of_several_servers);
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
