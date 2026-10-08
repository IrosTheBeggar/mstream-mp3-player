// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ui/Thumbs.h"

#include <Arduino.h>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <fcntl.h>
#include <lgfx/utility/lgfx_tjpgd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "CardContract.h"
#include "JpegInfo.h"
#include "LibraryIndex.h"
#include "app/CardWorker.h"
#include "app/Library.h"
#include "app/Psram.h"

namespace ui {

namespace {

constexpr size_t kChunk = 4096;     // a card access at most (the FAT lock and the SPI bus are the decoder's too)
constexpr size_t kPoolBytes = 4096; // TJpgDec's work area (M5GFX gives it 3,900)

uint32_t internalFree() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); }

}  // namespace

// The one job in flight: filled by the loop (prepare()), worked on by the
// worker, read back by the loop (finish()). The state word hands it over:
// only loads and stores (it is in PSRAM, where the ESP32 can't do atomic
// read-modify-writes).
struct Thumbs::Job {
  std::atomic<uint8_t> state{Idle};
  // In.
  uint32_t album = kNone;
  uint32_t generation = 0;
  uint8_t sizes = 0;          // wanted (ThumbCache::bit)
  bool pickLargest = false;   // no well-named cover: the largest .jpg in `folder`
  bool skipCard = false;      // decode even if the card has a copy (console uiT: timings)
  bool transfer = false;      // the album has the transfer's thumbnail (kTransferThumb)
  uint64_t folderHash = 0;    // ... keyed by its album folder's path hash (2.14.1)
  char root[16] = "";         // the VFS mount point
  char thumbDir[48] = "";     // "<root>/.player/thumbs"
  char transferDir[48] = "";  // "<root>/.mstream/thumbs"
  char image[288] = "";       // the index's cover ("/music/.../cover.jpg"): the card copy's key; "" none
  char folder[256] = "";
  // Out.
  bool ok = false;
  uint8_t made = 0;           // sizes in pixels[] (bits)
  bool noPicture = false;     // it can't be decoded (and is remembered as such on the card)
  bool known = false;         // ... the card said so already
  bool fromCard = false;
  bool fromTransfer = false;
  bool progressive = false;
  uint16_t width = 0, height = 0;
  uint8_t scale = 0;
  uint32_t bytes = 0;
  uint32_t ms = 0;
  uint32_t readMs = 0, decodeMs = 0, writeMs = 0;  // ms's parts: the header and the file in, TJpgDec + scaler, the card copy
  uint32_t internalMin = 0;
  char used[288] = "";        // the file decoded
  char note[64] = "";
  // The worker's buffers (PSRAM, kept).
  uint16_t* pixels[2] = {nullptr, nullptr};
  uint8_t* pool = nullptr;
  uint8_t* input = nullptr;   // the JPEG's input, kInputBytes
  ThumbScaler* scaler = nullptr;
  char path[320] = "";        // scratch: VFS paths
  char tmp[320] = "";
};

namespace {

using Job = Thumbs::Job;

// ---- the worker's side ----

bool readAll(int fd, void* dst, size_t n) {
  auto* p = static_cast<uint8_t*>(dst);
  while (n) {
    const size_t want = std::min(n, kChunk);
    const ssize_t got = read(fd, p, want);
    if (got <= 0) return false;
    p += got;
    n -= static_cast<size_t>(got);
  }
  return true;
}

bool writeAll(int fd, const void* src, size_t n) {
  const auto* p = static_cast<const uint8_t*>(src);
  while (n) {
    const size_t k = std::min(n, kChunk);
    const ssize_t put = write(fd, p, k);
    if (put <= 0) return false;
    p += put;
    n -= static_cast<size_t>(put);
  }
  return true;
}

// A thumbnail file (MPTH v1: the device's own copy, or the transfer's) at
// `path` for `hash`: the sizes asked, or that it can't be decoded. False:
// none (or not ours: a hash clash, another version).
bool readThumbFile(Job& j, const char* path, uint64_t hash, bool* noPicture) {
  const int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  uint8_t head[thumbfile::kHeaderBytes];
  thumbfile::Header h;
  bool ok = readAll(fd, head, sizeof(head)) && thumbfile::read(head, sizeof(head), &h) && h.pathHash == hash;
  *noPicture = false;
  if (ok && (h.flags & thumbfile::kNoPicture)) {
    *noPicture = true;
  } else if (ok) {
    j.made = 0;
    for (int k = 0; k < 2 && ok; ++k) {
      const auto s = static_cast<ThumbCache::Size>(k);
      if (!(j.sizes & ThumbCache::sizeBit(s))) continue;
      ok = lseek(fd, static_cast<off_t>(thumbfile::offsetOf(s)), SEEK_SET) >= 0 &&
           readAll(fd, j.pixels[k], ThumbCache::slotBytes(s));
      if (ok) j.made = static_cast<uint8_t>(j.made | ThumbCache::sizeBit(s));
    }
  }
  close(fd);
  return ok;
}

// The card's copy of this cover's thumbnails (/.player/thumbs).
bool readCardCopy(Job& j, uint64_t hash) {
  if (!thumbfile::path(j.thumbDir, hash, j.path, sizeof(j.path))) return false;
  bool noPicture = false;
  const bool ok = readThumbFile(j, j.path, hash, &noPicture);
  if (ok && noPicture) {
    j.noPicture = j.known = true;
  } else if (ok) {
    j.ok = true;
  }
  j.fromCard = ok;
  return ok;
}

// The transfer's thumbnail (2.14.1): /.mstream/thumbs, read only. One that
// says "no picture" isn't the software's (it never writes one): a miss.
bool readTransferThumb(Job& j) {
  if (!cardcontract::transferThumbPath(j.transferDir, j.folderHash, j.path, sizeof(j.path))) return false;
  bool noPicture = false;
  const bool ok = readThumbFile(j, j.path, j.folderHash, &noPicture) && !noPicture;
  if (ok) {
    j.ok = true;
    j.fromTransfer = true;
    snprintf(j.used, sizeof(j.used), "%s", j.path);
  }
  return ok;
}

// Written aside, then renamed over: a cut-off write never looks whole.
void writeCardCopy(Job& j, uint64_t hash, uint32_t sourceBytes, bool noPicture) {
  mkdir(j.thumbDir, 0777);  // EEXIST is fine
  if (!thumbfile::path(j.thumbDir, hash, j.path, sizeof(j.path), true)) return;
  mkdir(j.path, 0777);
  if (!thumbfile::path(j.thumbDir, hash, j.path, sizeof(j.path))) return;
  snprintf(j.tmp, sizeof(j.tmp), "%s", j.path);
  const size_t n = strlen(j.tmp);
  if (n < 4) return;
  memcpy(j.tmp + n - 4, ".tmp", 4);  // XXXXXXXX.tmp: still an 8.3 name
  const int fd = open(j.tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) return;
  thumbfile::Header h;
  h.pathHash = hash;
  h.sourceBytes = sourceBytes;
  h.flags = noPicture ? thumbfile::kNoPicture : 0;
  uint8_t head[thumbfile::kHeaderBytes];
  thumbfile::write(h, head);
  bool ok = writeAll(fd, head, sizeof(head));
  if (ok && !noPicture) {
    ok = writeAll(fd, j.pixels[0], ThumbCache::slotBytes(ThumbCache::Size::Small)) &&
         writeAll(fd, j.pixels[1], ThumbCache::slotBytes(ThumbCache::Size::Large));
  }
  close(fd);
  if (ok) {
    unlink(j.path);
    ok = rename(j.tmp, j.path) == 0;
  }
  if (!ok) unlink(j.tmp);
}

// The largest .jpg in the cover's folder (no cover.jpg, folder.jpg or
// front.jpg there, and more than one other): into j.used.
void pickLargest(Job& j) {
  snprintf(j.path, sizeof(j.path), "%s%s", j.root, j.folder);
  DIR* d = opendir(j.path);
  if (!d) return;
  const size_t base = strlen(j.path);
  off_t best = -1;
  while (const dirent* e = readdir(d)) {
    const char* name = e->d_name;
    if (name[0] == '.' || LibraryIndex::imageRank(name, strlen(name)) == LibraryIndex::kNoImage) continue;
    if (base + 1 + strlen(name) + 1 > sizeof(j.path)) continue;
    j.path[base] = '/';
    strcpy(j.path + base + 1, name);
    struct stat st;
    if (stat(j.path, &st) == 0 && st.st_size > best) {
      best = st.st_size;
      snprintf(j.used, sizeof(j.used), "%s/%s", j.folder, name);
    }
    j.path[base] = 0;
  }
  closedir(d);
}

// The JPEG streamed from its file: TJpgDec asks a few hundred bytes at a
// time; they come from `input`, refilled from the card in kInputBytes
// reads (multi-sector reads, not one card access per TJpgDec request).
struct Stream {
  int fd;
  uint8_t* buf;
  uint32_t fill;  // valid bytes in buf
  uint32_t pos;   // the next one
  bool failed;
  ThumbScaler* scaler;
  uint32_t outputs;
  uint32_t* internalMin;
};

bool refill(Stream& s) {
  const ssize_t got = read(s.fd, s.buf, Thumbs::kInputBytes);
  if (got < 0) s.failed = true;
  s.fill = got > 0 ? static_cast<uint32_t>(got) : 0;
  s.pos = 0;
  return s.fill > 0;
}

uint32_t jpegIn(void* dev, uint8_t* buf, uint32_t len) {
  auto* s = static_cast<Stream*>(dev);
  uint32_t done = 0;
  while (done < len) {
    if (s->pos >= s->fill && !refill(*s)) break;
    const uint32_t k = std::min(len - done, s->fill - s->pos);
    if (buf) memcpy(buf + done, s->buf + s->pos, k);
    s->pos += k;
    done += k;
  }
  return done;
}

uint32_t jpegOut(void* dev, void* bitmap, JRECT* r) {
  auto* s = static_cast<Stream*>(dev);
  s->scaler->add(static_cast<int>(r->left), static_cast<int>(r->top), static_cast<int>(r->right - r->left + 1),
                 static_cast<int>(r->bottom - r->top + 1), static_cast<const uint8_t*>(bitmap));
  if ((++s->outputs & 63) == 0) *s->internalMin = std::min(*s->internalMin, internalFree());
  return 1;
}

const char* jresName(JRESULT r) {
  switch (r) {
    case JDR_INP: return "the file ended early";
    case JDR_MEM1: return "not enough work memory";
    case JDR_MEM2: return "the input buffer is too small";
    case JDR_FMT1: return "damaged data";
    case JDR_FMT2: return "a format it doesn't support";
    case JDR_FMT3: return "a JPEG standard it doesn't support";
    default: return "an error";
  }
}

// The header walk's reads (jpeg::parseFile()): at offsets of the open file.
bool readAt(uint32_t offset, uint8_t* out, uint32_t n, void* ctx) {
  const int fd = *static_cast<const int*>(ctx);
  return lseek(fd, static_cast<off_t>(offset), SEEK_SET) >= 0 && readAll(fd, out, n);
}

// Decodes j.used, streamed from the card, into both sizes. False: j.note says why.
bool decode(Job& j, uint32_t* sourceBytes, bool* undecodable) {
  *undecodable = false;
  snprintf(j.path, sizeof(j.path), "%s%s", j.root, j.used);
  int fd = open(j.path, O_RDONLY);
  if (fd < 0) {
    snprintf(j.note, sizeof(j.note), "can't open it");
    return false;
  }
  struct stat st;
  const bool sized = fstat(fd, &st) == 0;
  const size_t size = sized ? static_cast<size_t>(st.st_size) : 0;
  *sourceBytes = static_cast<uint32_t>(size);
  j.bytes = static_cast<uint32_t>(size);
  if (!sized || size < 64) {
    close(fd);
    snprintf(j.note, sizeof(j.note), "too small (%u bytes)", static_cast<unsigned>(size));
    *undecodable = sized;
    return false;
  }
  // Its frame header, wherever it is (EXIF, XMP, a Photoshop block and an
  // ICC profile can put it past 64 KB): segments skipped by their lengths.
  const jpeg::Info info = jpeg::parseFile(readAt, &fd, static_cast<uint32_t>(size));
  const int64_t readAt0 = esp_timer_get_time();
  j.internalMin = std::min(j.internalMin, internalFree());
  bool ok = false;
  if (info.readFailed) {
    snprintf(j.note, sizeof(j.note), "a read failed");  // the card, not the picture: tried again later
  } else if (!info.ok) {
    snprintf(j.note, sizeof(j.note), "not a JPEG the decoder can read");
    *undecodable = true;
  } else if (info.progressive) {
    j.progressive = true;
    j.width = info.width;
    j.height = info.height;
    snprintf(j.note, sizeof(j.note), "progressive JPEG (the decoder reads baseline only)");
    *undecodable = true;
  } else if (lseek(fd, 0, SEEK_SET) < 0) {
    snprintf(j.note, sizeof(j.note), "a read failed");
  } else {
    j.width = info.width;
    j.height = info.height;
    const int scale = ThumbScaler::decoderScale(info.width, info.height, ThumbCache::kLargePx);
    j.scale = static_cast<uint8_t>(scale);
    Stream s{fd, j.input, 0, 0, false, j.scaler, 0, &j.internalMin};
    lgfxJdec jd;
    JRESULT r = lgfx_jd_prepare(&jd, jpegIn, j.pool, kPoolBytes, &s);
    static const int kSizes[2] = {ThumbCache::kSmallPx, ThumbCache::kLargePx};
    if (r == JDR_OK && !j.scaler->begin(jd.width >> scale, jd.height >> scale, kSizes, 2)) {
      snprintf(j.note, sizeof(j.note), "no PSRAM for the scaler");
    } else if (r == JDR_OK && (r = lgfx_jd_decomp(&jd, jpegOut, static_cast<uint_fast8_t>(scale))) == JDR_OK) {
      j.scaler->finish(0, j.pixels[0]);
      j.scaler->finish(1, j.pixels[1]);
      j.made = ThumbCache::sizeBit(ThumbCache::Size::Small) | ThumbCache::sizeBit(ThumbCache::Size::Large);
      ok = true;
    } else if (s.failed) {
      snprintf(j.note, sizeof(j.note), "a read failed");  // the card, not the picture: tried again later
    } else {
      snprintf(j.note, sizeof(j.note), "the decoder: %s", jresName(r));
      *undecodable = r != JDR_MEM1;
    }
    j.scaler->end();
  }
  close(fd);
  j.decodeMs = static_cast<uint32_t>((esp_timer_get_time() - readAt0) / 1000);
  return ok;
}

void run(Job& j) {
  const int64_t t0 = esp_timer_get_time();
  j.ok = j.noPicture = j.known = j.fromCard = j.fromTransfer = j.progressive = false;
  j.made = 0;
  j.width = j.height = 0;
  j.scale = 0;
  j.bytes = 0;
  j.note[0] = 0;
  j.internalMin = internalFree();
  snprintf(j.used, sizeof(j.used), "%s", j.image);
  j.readMs = j.decodeMs = j.writeMs = 0;
  // 1. The transfer's thumbnail (2.14.3, step 1).
  if (j.transfer && readTransferThumb(j)) {
    j.ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
    j.readMs = j.ms;
    return;
  }
  // 2. The folder's image: the card's copy, else decoded (and copied).
  if (!j.image[0]) {
    snprintf(j.note, sizeof(j.note), "the transfer's thumbnail is missing or damaged, and no image");
    j.ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
    return;
  }
  const uint64_t hash = thumbfile::pathHash(j.image);
  if (j.skipCard || !readCardCopy(j, hash)) {
    if (j.pickLargest) pickLargest(j);
    uint32_t sourceBytes = 0;
    bool undecodable = false;
    j.ok = decode(j, &sourceBytes, &undecodable);
    const int64_t decodedAt = esp_timer_get_time();
    j.readMs = static_cast<uint32_t>((decodedAt - t0) / 1000) - j.decodeMs;
    if (j.ok) {
      writeCardCopy(j, hash, sourceBytes, false);
    } else if (undecodable) {
      j.noPicture = true;
      writeCardCopy(j, hash, sourceBytes, true);  // so the next boot doesn't try again
    }
    j.writeMs = static_cast<uint32_t>((esp_timer_get_time() - decodedAt) / 1000);
  }
  j.internalMin = std::min(j.internalMin, internalFree());
  j.ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
}

}  // namespace

// ---- the loop's side ----

bool Thumbs::begin() {
  if (ready_) return true;
  if (!cache_.begin(kSmallSlots, kLargeSlots)) return false;
  job_ = psramNew<Job>();
  if (!job_) return false;
  job_->pixels[0] = static_cast<uint16_t*>(psramAlloc(ThumbCache::slotBytes(Size::Small)));
  job_->pixels[1] = static_cast<uint16_t*>(psramAlloc(ThumbCache::slotBytes(Size::Large)));
  job_->pool = static_cast<uint8_t*>(psramAlloc(kPoolBytes));
  job_->input = static_cast<uint8_t*>(psramAlloc(kInputBytes));
  job_->scaler = psramNew<ThumbScaler>(psramAlloc, psramFree);
  if (!job_->pixels[0] || !job_->pixels[1] || !job_->pool || !job_->input || !job_->scaler) return false;
  ready_ = true;
  return true;
}

bool Thumbs::hasCover(uint32_t album) const {
  const LibraryIndex* idx = const_cast<Library&>(library_).index();
  if (!ready_ || !idx || !idx->ready() || album >= idx->albumCount()) return false;
  return idx->albumCover(album) != LibraryIndex::kNone || (idx->album(album).flags & LibraryIndex::kTransferThumb);
}

const uint16_t* Thumbs::get(uint32_t album, Size s) {
  if (!ready_ || album == kNone) return nullptr;
  const uint16_t* px = cache_.get(album, s);
  if (!px && hasCover(album)) cache_.want(album, s);
  return px;
}

void Thumbs::libraryChanged() {
  ++generation_;
  cache_.clear();
}

void Thumbs::redecode() {
  libraryChanged();
  skipCard_ = true;
  Serial.println("[thumb] the covers are decoded again this session (the card's copies are rewritten)");
}

void Thumbs::stepEntry(void* self) {
  Job& j = *static_cast<Thumbs*>(self)->job_;
  j.state.store(Working);
  run(j);
  j.state.store(Done);
}

bool Thumbs::prepare(uint32_t album, uint8_t sizes) {
  LibraryIndex* idx = library_.index();
  if (!idx || !idx->ready() || album >= idx->albumCount()) return false;
  LocalStorage& storage = library_.storage();
  if (!storage.available()) return false;
  Job& j = *job_;
  const LibraryIndex::Album& a = idx->album(album);
  j.transfer = (a.flags & LibraryIndex::kTransferThumb) != 0;
  j.image[0] = 0;
  j.folder[0] = 0;
  j.pickLargest = false;
  if (j.transfer) {
    // Its key: the album folder's path hash (2.14.1).
    char folder[256];
    if (!idx->folderPath(a.folder, folder, sizeof(folder))) return false;
    j.folderHash = thumbfile::pathHash(folder);
  }
  const uint32_t f = idx->albumCover(album);
  if (f != LibraryIndex::kNone) {
    if (!idx->imagePath(f, j.image, sizeof(j.image)) || !idx->folderPath(f, j.folder, sizeof(j.folder))) return false;
    const LibraryIndex::Folder& folder = idx->folder(f);
    j.pickLargest = folder.imageRank == 3 && folder.imageCount > 1;
  } else if (!j.transfer) {
    return false;
  }
  snprintf(j.root, sizeof(j.root), "%s", storage.vfsRoot());
  snprintf(j.thumbDir, sizeof(j.thumbDir), "%s%s/thumbs", storage.vfsRoot(), storage.stateDir());
  snprintf(j.transferDir, sizeof(j.transferDir), "%s/.mstream/thumbs", storage.vfsRoot());
  j.album = album;
  j.sizes = sizes;
  j.skipCard = skipCard_;
  j.generation = generation_;
  return true;
}

void Thumbs::finish(uint32_t* arrived) {
  Job& j = *job_;
  const uint32_t album = j.album;
  internalMin_ = std::min(internalMin_, j.internalMin);
  if (j.generation == generation_) {
    if (j.ok) {
      for (int k = 0; k < 2; ++k) {
        const auto s = static_cast<Size>(k);
        if (j.made & ThumbCache::sizeBit(s)) cache_.put(album, s, j.pixels[k]);
      }
      *arrived = album;
    } else {
      cache_.markFailed(album);  // the placeholder, and not asked again this session
    }
    cache_.done(album);
  }
  if (j.ok && j.fromTransfer) {
    ++fromTransfer_;
  } else if (j.ok && j.fromCard) {
    ++fromCard_;
  } else if (j.ok) {
    ++decoded_;
    decodeMsSum_ += j.ms;
    decodeMsMax_ = std::max(decodeMsMax_, j.ms);
    Serial.printf("[thumb] %s: %ux%u at 1/%d, 40 + 96 px in %lu ms (header and reads %lu, decode %lu, card copy %lu; "
                  "%lu KB streamed); internal RAM %lu B free at the lowest\n",
                  j.used, j.width, j.height, 1 << j.scale, (unsigned long)j.ms, (unsigned long)j.readMs,
                  (unsigned long)j.decodeMs, (unsigned long)j.writeMs, (unsigned long)(j.bytes / 1024),
                  (unsigned long)j.internalMin);
  } else if (!j.known) {
    // Said once: a cover that can't be decoded is remembered (here, and on the card).
    ++failures_;
    if (j.progressive) ++progressive_;
    Serial.printf("[thumb] %s: %s%s: the placeholder\n", j.used[0] ? j.used : j.image, j.note[0] ? j.note : "failed",
                  j.noPicture ? " (remembered on the card)" : "");
  }
  j.state.store(Idle);
}

uint32_t Thumbs::loop(uint32_t nowMs) {
  (void)nowMs;
  if (!ready_) return kNone;
  uint32_t arrived = kNone;
  if (job_->state.load() == Done) finish(&arrived);
  return arrived;
}

bool Thumbs::wantsCover(uint32_t nowMs) const {
  return ready_ && job_->state.load() == Idle && cache_.wanted() > 0 && cache_.making() == kNone &&
         static_cast<int32_t>(nowMs - retryAtMs_) >= 0;
}

bool Thumbs::startCover(CardWorker& worker, uint8_t priority, uint32_t nowMs) {
  if (!wantsCover(nowMs)) return false;
  uint32_t album;
  uint8_t sizes;
  if (!cache_.next(&album, &sizes)) return false;
  if (!prepare(album, sizes)) {
    cache_.markFailed(album);
    cache_.done(album);
    return false;
  }
  job_->state.store(Queued);
  if (!worker.start(CardWorker::Job::Cover, priority, stepEntry, this, nowMs)) {
    job_->state.store(Idle);
    cache_.done(album);  // asked again when a row draws it
    retryAtMs_ = nowMs + 1000;
    return false;
  }
  return true;
}

void Thumbs::printState() const {
  if (!ready_) {
    Serial.println("[thumb] off (no PSRAM for the cache)");
    return;
  }
  const ThumbCache::Stats& s = cache_.stats();
  Serial.printf("[thumb] cache %u KB PSRAM (%lu small, %lu large slots): %lu hits, %lu misses, %lu stored, %lu "
                "evicted; %lu wanted\n",
                static_cast<unsigned>(cache_.bytes() / 1024), (unsigned long)cache_.slots(Size::Small),
                (unsigned long)cache_.slots(Size::Large), (unsigned long)s.hits, (unsigned long)s.misses,
                (unsigned long)s.stored, (unsigned long)s.evicted, (unsigned long)cache_.wanted());
  Serial.printf("[thumb] made: %lu decoded (mean %lu ms, max %lu), %lu from the card, %lu transfer thumbnails, %lu "
                "failed (%lu progressive); internal RAM during jobs %s%lu B at the lowest (the card worker: gs)\n",
                (unsigned long)decoded_, (unsigned long)(decoded_ ? decodeMsSum_ / decoded_ : 0),
                (unsigned long)decodeMsMax_, (unsigned long)fromCard_, (unsigned long)fromTransfer_,
                (unsigned long)failures_, (unsigned long)progressive_, internalMin_ == UINT32_MAX ? "(none yet) " : "",
                (unsigned long)(internalMin_ == UINT32_MAX ? 0 : internalMin_));
}

}  // namespace ui
