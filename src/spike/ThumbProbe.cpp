// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "spike/ThumbProbe.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <climits>
#include <cstring>

#include "Percentiles.h"
#include "spike/SpikeUi.h"

using namespace spike;

namespace {

const char* const kCoverNames[] = {"cover.jpg", "Cover.jpg", "folder.jpg", "Folder.jpg", "front.jpg"};
constexpr const char* kCacheDir = "/uispike";

// Lowest free internal heap seen at each of the decoder's reads.
struct Sampler {
  uint32_t minFree = UINT32_MAX;
  uint32_t reads = 0;
  uint32_t maxReadUs = 0;
  void sample() {
    const auto f = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (f < minFree) minFree = f;
    ++reads;
  }
};

struct MemoryReader : public lgfx::PointerWrapper {
  Sampler* sampler;
  MemoryReader(const uint8_t* p, uint32_t n, Sampler* s) : lgfx::PointerWrapper(p, n), sampler(s) {}
  int read(uint8_t* buf, uint32_t len) override {
    sampler->sample();
    return lgfx::PointerWrapper::read(buf, len);
  }
};

// The decoder reading the card: TJpgDec asks for its input buffer's worth
// at a time (well under kChunk), each call timed.
struct FileReader : public lgfx::DataWrapper {
  fs::File file;
  Sampler* sampler;
  ThumbProbe::SdWatch* watch;
  FileReader(fs::File f, Sampler* s, ThumbProbe::SdWatch* w) : file(f), sampler(s), watch(w) {}
  int read(uint8_t* buf, uint32_t len) override {
    sampler->sample();
    const int64_t t0 = esp_timer_get_time();
    const int n = static_cast<int>(file.read(buf, len));
    const auto us = static_cast<uint32_t>(esp_timer_get_time() - t0);
    if (us > sampler->maxReadUs) sampler->maxReadUs = us;
    if (watch) watch->call(us);
    return n;
  }
  void skip(int32_t offset) override { file.seek(file.position() + offset); }
  bool seek(uint32_t offset) override { return file.seek(offset); }
  void close() override {}
  int32_t tell() override { return static_cast<int32_t>(file.position()); }
};

float msSince(int64_t t0) { return (esp_timer_get_time() - t0) / 1000.0f; }

void line(const char* s, int y, uint16_t colour = col::SOFT) {
  auto& d = M5.Display;
  d.setFont(&fonts::Font2);
  d.setTextColor(colour, col::BG);
  d.setTextDatum(textdatum_t::top_left);
  d.setTextPadding(kW - 6);
  d.drawString(s, 6, y);
  d.setTextPadding(0);
}

}  // namespace

void ThumbProbe::SdWatch::start(Core2AudioBackend* a) {
  *this = SdWatch{};
  audio = a;
  underruns0 = a ? a->underrunsNow() : 0;
  sampleRing();
}

void ThumbProbe::SdWatch::sampleRing() {
  if (!audio) return;
  const uint32_t r = audio->bufferedMsNow();
  if (r < ringMin) ringMin = r;
}

void ThumbProbe::SdWatch::call(uint32_t us) {
  ++calls;
  if (us > maxCallUs) maxCallUs = us;
  sampleRing();
}

uint32_t ThumbProbe::SdWatch::underruns() const { return audio ? audio->underrunsNow() - underruns0 : 0; }

void ThumbProbe::SdWatch::log(const char* step) const {
  const bool playing = audio && audio->isPlaying();
  Serial.printf("[thumb] SD side, %s: %lu card calls (<= %lu B each), longest %.1f ms; ring min %lu ms, underruns "
                "+%lu%s\n",
                step, (unsigned long)calls, (unsigned long)kChunk, maxCallUs / 1000.0f,
                (unsigned long)(ringMin == UINT32_MAX ? 0 : ringMin), (unsigned long)underruns(),
                playing ? "" : " (nothing playing: start a track with i<n> for the real cost)");
}

uint32_t ThumbProbe::readChunked(fs::File& f, uint8_t* dst, uint32_t len, SdWatch* watch) {
  uint32_t got = 0;
  while (got < len) {
    const uint32_t want = std::min(kChunk, len - got);
    const int64_t t0 = esp_timer_get_time();
    const auto n = static_cast<uint32_t>(f.read(dst + got, want));
    if (watch) watch->call(static_cast<uint32_t>(esp_timer_get_time() - t0));
    got += n;
    if (n != want) break;
  }
  return got;
}

uint32_t ThumbProbe::writeChunked(fs::File& f, const uint8_t* src, uint32_t len, SdWatch* watch) {
  uint32_t put = 0;
  while (put < len) {
    const uint32_t want = std::min(kChunk, len - put);
    const int64_t t0 = esp_timer_get_time();
    const auto n = static_cast<uint32_t>(f.write(src + put, want));
    if (watch) watch->call(static_cast<uint32_t>(esp_timer_get_time() - t0));
    put += n;
    if (n != want) break;
  }
  return put;
}

ThumbProbe::~ThumbProbe() {
  thumb40_.deleteSprite();
  thumb80_.deleteSprite();
  psramFree(cache40_);
  psramFree(cache80_);
}

bool ThumbProbe::ensure() {
  if (ready_) return true;
  for (M5Canvas* s : {&thumb40_, &thumb80_}) {
    s->setPsram(true);  // before createSprite(): otherwise internal RAM
    s->setColorDepth(16);
  }
  ready_ = thumb40_.createSprite(40, 40) && thumb80_.createSprite(80, 80);
  cache40_ = static_cast<uint16_t*>(psramAlloc(40 * 40 * 2));
  cache80_ = static_cast<uint16_t*>(psramAlloc(80 * 80 * 2));
  ready_ = ready_ && cache40_ && cache80_;
  if (!ready_) Serial.println("[thumb] no PSRAM for the probe");
  return ready_;
}

void ThumbProbe::close() {
  if (!active_) return;
  active_ = false;
  Serial.println("[thumb] closed");
}

bool ThumbProbe::coverPath(fs::FS& fs, const LibraryIndex* index, uint32_t album, char* buf, size_t size) const {
  const size_t n = index->folderPath(index->album(album).folder, buf, size);
  if (n == 0) return false;
  for (const char* name : kCoverNames) {
    if (n + 1 + strlen(name) + 1 > size) return false;
    buf[n] = '/';
    strcpy(buf + n + 1, name);
    if (fs.exists(buf)) return true;
  }
  return false;
}

// Size and the frame header (SOFn): width, height, progressive.
bool ThumbProbe::parseJpeg(const uint8_t* p, uint32_t len, Jpeg* out) {
  out->bytes = len;
  if (len < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;
  uint32_t i = 2;
  while (i + 4 <= len) {
    if (p[i] != 0xFF) {
      ++i;
      continue;
    }
    const uint8_t m = p[i + 1];
    if (m == 0xFF) {
      ++i;
      continue;
    }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
      i += 2;
      continue;
    }
    const uint32_t segLen = (p[i + 2] << 8) | p[i + 3];
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      if (i + 9 > len) return false;
      out->height = (p[i + 5] << 8) | p[i + 6];
      out->width = (p[i + 7] << 8) | p[i + 8];
      out->progressive = m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE;
      return true;
    }
    if (m == 0xD9 || m == 0xDA) return false;  // end, or scan data before any frame header
    i += 2 + segLen;
  }
  return false;
}

ThumbProbe::Decode ThumbProbe::decodeFromMemory(M5Canvas& dst, int size, const uint8_t* data, uint32_t len,
                                                const Jpeg& j) {
  Decode r;
  dst.fillSprite(col::CARD);
  const float scale = static_cast<float>(size) / std::max(j.width, j.height);
  Sampler s;
  MemoryReader in(data, len, &s);
  const uint32_t before = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  const int64_t t0 = esp_timer_get_time();
  r.ok = dst.drawJpg(&in, (size - static_cast<int>(j.width * scale)) / 2,
                     (size - static_cast<int>(j.height * scale)) / 2, size, size, 0, 0, scale, scale);
  r.ms = msSince(t0);
  r.reads = s.reads;
  r.internalPeak = s.minFree == UINT32_MAX || s.minFree > before ? 0 : before - s.minFree;
  return r;
}

ThumbProbe::Decode ThumbProbe::decodeFromFile(M5Canvas& dst, int size, fs::FS& fs, const char* path, const Jpeg& j,
                                              SdWatch* watch) {
  Decode r;
  dst.fillSprite(col::CARD);
  const float scale = static_cast<float>(size) / std::max(j.width, j.height);
  Sampler s;
  const uint32_t before = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  const int64_t t0 = esp_timer_get_time();
  fs::File f = fs.open(path, "r");
  if (!f) return r;
  {
    FileReader in(f, &s, watch);
    r.ok = dst.drawJpg(&in, (size - static_cast<int>(j.width * scale)) / 2,
                       (size - static_cast<int>(j.height * scale)) / 2, size, size, 0, 0, scale, scale);
  }
  f.close();
  r.ms = msSince(t0);
  r.reads = s.reads;
  r.maxReadUs = s.maxReadUs;
  r.internalPeak = s.minFree == UINT32_MAX || s.minFree > before ? 0 : before - s.minFree;
  return r;
}

void ThumbProbe::probe(fs::FS& fs, const LibraryIndex* index, uint32_t album, bool write) {
  char path[256];
  if (!coverPath(fs, index, album, path, sizeof(path))) {
    Serial.printf("[thumb] album %lu \"%s\": no cover.jpg / folder.jpg\n", (unsigned long)album,
                  index->albumName(album));
    return;
  }
  auto& d = M5.Display;
  fillLcd(0, kH, col::BG);
  char msg[120];
  snprintf(msg, sizeof(msg), "%s / %s", index->artistName(index->album(album).artist), index->albumName(album));
  char folded[64];
  d.setFont(&fonts::Font2);
  fitText(d, msg, strlen(msg), folded, sizeof(folded), kW - 12);
  line(folded, 4, col::TXT);

  // 1. The file into PSRAM (the SD card's part), kChunk bytes a call.
  SdWatch w;
  w.start(audio_);
  int64_t t0 = esp_timer_get_time();
  fs::File f = fs.open(path, "r");
  const uint32_t len = f ? static_cast<uint32_t>(f.size()) : 0;
  auto* data = static_cast<uint8_t*>(len ? psramAlloc(len) : nullptr);
  const uint32_t got = data ? readChunked(f, data, len, &w) : 0;
  if (f) f.close();
  const float readMs = msSince(t0);
  w.log("reading the file into PSRAM");
  Jpeg j;
  if (!data || got != len || !parseJpeg(data, len, &j) || j.width == 0 || j.height == 0) {
    Serial.printf("[thumb] %s: can't read it or no JPEG frame header (%lu bytes)\n", path, (unsigned long)len);
    psramFree(data);
    return;
  }
  Serial.printf("[thumb] %s: %lu bytes, %dx%d%s; read into PSRAM in %.1f ms (%.0f KB/s)\n", path, (unsigned long)len,
                j.width, j.height, j.progressive ? " PROGRESSIVE (TJpgDec can't decode it)" : " baseline", readMs,
                len / 1.024f / readMs);

  // 2. Decode: from the SD card (the real case) and from the PSRAM copy.
  const uint32_t largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  w.start(audio_);
  const Decode f40 = decodeFromFile(thumb40_, 40, fs, path, j, &w);
  const Decode f80 = decodeFromFile(thumb80_, 80, fs, path, j, &w);
  w.log("decoding from the card (40 and 80)");
  const Decode m40 = decodeFromMemory(thumb40_, 40, data, len, j);
  const Decode m80 = decodeFromMemory(thumb80_, 80, data, len, j);
  psramFree(data);
  Serial.printf("[thumb] decode from SD:    40x40 %s %.1f ms, 80x80 %s %.1f ms (%lu/%lu reads, longest %.2f/%.2f ms)\n",
                f40.ok ? "ok" : "FAILED", f40.ms, f80.ok ? "ok" : "FAILED", f80.ms, (unsigned long)f40.reads,
                (unsigned long)f80.reads, f40.maxReadUs / 1000.0f, f80.maxReadUs / 1000.0f);
  Serial.printf("[thumb] decode from PSRAM: 40x40 %s %.1f ms, 80x80 %s %.1f ms\n", m40.ok ? "ok" : "FAILED", m40.ms,
                m80.ok ? "ok" : "FAILED", m80.ms);
  Serial.printf("[thumb] internal RAM taken while decoding: %lu B from SD, %lu B from PSRAM (largest free block "
                "before: %lu B; TJpgDec's pool is 3900 B)\n",
                (unsigned long)std::max(f40.internalPeak, f80.internalPeak),
                (unsigned long)std::max(m40.internalPeak, m80.internalPeak), (unsigned long)largest);

  // 3. Cache in PSRAM, and redraw from the cache.
  t0 = esp_timer_get_time();
  memcpy(cache40_, thumb40_.getBuffer(), 40 * 40 * 2);
  memcpy(cache80_, thumb80_.getBuffer(), 80 * 80 * 2);
  const float cacheMs = msSince(t0);
  lock_.reset();
  t0 = esp_timer_get_time();
  {
    LcdLock lock(&lock_);
    d.pushImage(20, 40, 40, 40, reinterpret_cast<const lgfx::swap565_t*>(cache40_));
  }
  const float push40 = msSince(t0);
  t0 = esp_timer_get_time();
  {
    LcdLock lock(&lock_);
    d.pushImage(80, 40, 80, 80, reinterpret_cast<const lgfx::swap565_t*>(cache80_));
  }
  const float push80 = msSince(t0);
  Serial.printf("[thumb] PSRAM cache: copy %.3f ms (%u + %u B); redraw 40x40 %.2f ms, 80x80 %.2f ms (SPI held max "
                "%.2f ms)\n",
                cacheMs, 40 * 40 * 2, 80 * 80 * 2, push40, push80, lock_.maxUs / 1000.0f);

  snprintf(msg, sizeof(msg), "%dx%d %lu KB, read %.0f ms", j.width, j.height, (unsigned long)(len / 1024), readMs);
  line(msg, 130);
  snprintf(msg, sizeof(msg), "SD->40: %.0f ms  SD->80: %.0f ms", f40.ms, f80.ms);
  line(msg, 146);
  snprintf(msg, sizeof(msg), "mem->40: %.0f ms  mem->80: %.0f ms", m40.ms, m80.ms);
  line(msg, 162);
  snprintf(msg, sizeof(msg), "decoder internal RAM: %lu B",
           (unsigned long)std::max(std::max(f40.internalPeak, f80.internalPeak),
                                   std::max(m40.internalPeak, m80.internalPeak)));
  line(msg, 178);
  snprintf(msg, sizeof(msg), "from cache: 40 %.2f ms, 80 %.2f ms", push40, push80);
  line(msg, 194);

  // 4. Optionally: raw .565 files on the card, and a redraw from them.
  if (write) {
    fs.mkdir(kCacheDir);
    struct Item {
      int size;
      const uint16_t* px;
      int x;
    } items[] = {{40, cache40_, 180}, {80, cache80_, 230}};
    for (const Item& it : items) {
      char file[64];
      snprintf(file, sizeof(file), "%s/%lu_%d.565", kCacheDir, (unsigned long)album, it.size);
      const uint32_t bytes = static_cast<uint32_t>(it.size * it.size * 2);
      w.start(audio_);
      t0 = esp_timer_get_time();
      fs::File wf = fs.open(file, "w");
      bool ok = static_cast<bool>(wf);
      if (ok) {
        const uint8_t header[8] = {'T', '5', '6', '5', static_cast<uint8_t>(it.size), 0, static_cast<uint8_t>(it.size),
                                   0};
        ok = wf.write(header, 8) == 8 &&
             writeChunked(wf, reinterpret_cast<const uint8_t*>(it.px), bytes, &w) == bytes;
        wf.close();
      }
      const float writeMs = msSince(t0);
      auto* buf = static_cast<uint16_t*>(psramAlloc(bytes));
      t0 = esp_timer_get_time();
      fs::File r = fs.open(file, "r");
      uint8_t header[8];
      const bool readOk =
          buf && r && r.read(header, 8) == 8 && readChunked(r, reinterpret_cast<uint8_t*>(buf), bytes, &w) == bytes;
      if (r) r.close();
      const float readBackMs = msSince(t0);
      t0 = esp_timer_get_time();
      if (readOk) {
        LcdLock lock(&lock_);
        d.pushImage(it.x, 40, it.size, it.size, reinterpret_cast<const lgfx::swap565_t*>(buf));
      }
      const float pushMs = msSince(t0);
      psramFree(buf);
      Serial.printf("[thumb] %s: write %s %.1f ms; read back %s %.1f ms + push %.2f ms = %.1f ms\n", file,
                    ok ? "ok" : "FAILED", writeMs, readOk ? "ok" : "FAILED", readBackMs, pushMs, readBackMs + pushMs);
      w.log(it.size == 40 ? "the 40x40 .565 file (write, read back)" : "the 80x80 .565 file (write, read back)");
      if (it.size == 80) {
        snprintf(msg, sizeof(msg), "from .565 on SD: 80 %.1f ms (read %.1f)", readBackMs + pushMs, readBackMs);
        line(msg, 210);
      }
    }
  }
  line("40/80 decoded; PSRAM redraw (.565: right)", 224, col::FAINT);
}

void ThumbProbe::probeAll(fs::FS& fs, const LibraryIndex* index) {
  const uint32_t n = index->albumCount();
  float* ms = static_cast<float*>(psramAlloc(n * sizeof(float) + 4));
  float* peak = static_cast<float*>(psramAlloc(n * sizeof(float) + 4));
  if (!ms || !peak) {
    psramFree(ms);
    psramFree(peak);
    return;
  }
  uint32_t done = 0, missing = 0, failed = 0, progressive = 0;
  char path[256];
  SdWatch w;
  w.start(audio_);
  fillLcd(0, kH, col::BG);
  line("decoding every cover to 40x40...", 4, col::TXT);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t album = index->albumsAZ()[i];
    if (!coverPath(fs, index, album, path, sizeof(path))) {
      ++missing;
      continue;
    }
    // The header only (the first 64 KB covers any EXIF block), kChunk a call.
    fs::File f = fs.open(path, "r");
    const uint32_t len = f ? static_cast<uint32_t>(f.size()) : 0;
    const uint32_t head = std::min<uint32_t>(len, 65536);
    auto* buf = static_cast<uint8_t*>(head ? psramAlloc(head) : nullptr);
    const bool okRead = buf && readChunked(f, buf, head, &w) == head;
    if (f) f.close();
    Jpeg j;
    const bool okHead = okRead && parseJpeg(buf, head, &j) && j.width > 0;
    psramFree(buf);
    j.bytes = len;
    if (!okHead) {
      ++failed;
      continue;
    }
    if (j.progressive) {
      ++progressive;
      continue;
    }
    const Decode r = decodeFromFile(thumb40_, 40, fs, path, j, &w);
    if (!r.ok) {
      ++failed;
      continue;
    }
    ms[done] = r.ms;
    peak[done] = static_cast<float>(r.internalPeak);
    ++done;
    LcdLock lock;
    thumb40_.pushSprite(&M5.Display, 6 + (done - 1) % 7 * 44, 24 + ((done - 1) / 7) % 4 * 44);
  }
  const Percentiles p = Percentiles::of(ms, done);
  const Percentiles q = Percentiles::of(peak, done);
  Serial.printf("[thumb] all covers to 40x40 from SD: %lu decoded, %lu albums without a cover, %lu progressive "
                "(skipped), %lu failed; ms p50=%.1f p90=%.1f max=%.1f total=%.1f s; internal RAM peak p50=%.0f "
                "max=%.0f B\n",
                (unsigned long)done, (unsigned long)missing, (unsigned long)progressive, (unsigned long)failed, p.p50,
                p.p90, p.max, p.mean * done / 1000.0f, q.p50, q.max);
  w.log("every cover (headers and decodes)");
  char msg[80];
  snprintf(msg, sizeof(msg), "%lu covers: p50 %.0f ms, max %.0f ms", (unsigned long)done, p.p50, p.max);
  line(msg, 210, col::TXT);
  psramFree(ms);
  psramFree(peak);
}

void ThumbProbe::command(const char* arg, fs::FS* fs, const LibraryIndex* index) {
  if (arg && (arg[0] == 'q' || (!arg[0] && active_))) {
    close();
    return;
  }
  if (!fs) {
    Serial.println("[thumb] no SD card");
    return;
  }
  if (!index || !index->ready() || index->albumCount() == 0) {
    Serial.println("[thumb] no library index (g0 builds it from the SD card)");
    return;
  }
  if (!ensure()) return;
  active_ = true;
  if (arg && arg[0] == 'a') {
    probeAll(*fs, index);
    return;
  }
  const bool write = arg && arg[0] == 'w';
  const char* num = arg ? (write ? arg + 1 : arg) : "";
  if (*num) {
    const long i = atol(num);
    if (i < 0 || static_cast<uint32_t>(i) >= index->albumCount()) {
      Serial.printf("[thumb] album 0-%lu\n", (unsigned long)(index->albumCount() - 1));
      return;
    }
    probe(*fs, index, index->albumsAZ()[i], write);
    return;
  }
  char path[256];
  for (uint32_t i = 0; i < index->albumCount(); ++i) {
    const uint32_t album = index->albumsAZ()[i];
    if (coverPath(*fs, index, album, path, sizeof(path))) {
      Serial.printf("[thumb] album %lu of albums A-Z has the first cover\n", (unsigned long)i);
      probe(*fs, index, album, write);
      return;
    }
  }
  Serial.println("[thumb] no album folder has a cover.jpg (is the index the SD card's? g0)");
}
