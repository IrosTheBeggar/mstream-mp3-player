#include "spike/Spike.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>

#include <algorithm>

#include "LibrarySynth.h"
#include "spike/FontProbe.h"
#include "spike/InputLab.h"
#include "spike/ScrollLab.h"
#include "spike/SpikeUi.h"
#include "spike/ThumbProbe.h"

using namespace spike;

namespace {

uint32_t psramFreeNow() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)); }

// A command that brings a screen up (rather than asking for a report).
bool opens(const char* arg, bool active) {
  if (!arg || !*arg) return !active;
  return (arg[0] >= '0' && arg[0] <= '9') || arg[0] == 'a' || arg[0] == 'w';
}

struct WalkCtx {
  LibraryIndex* index;
  uint32_t files = 0, added = 0;
  uint64_t addUs = 0;
  uint32_t minFree = UINT32_MAX;
};

void onFile(const char* path, void* p) {
  auto& c = *static_cast<WalkCtx*>(p);
  ++c.files;
  const int64_t t0 = esp_timer_get_time();
  if (c.index->addFile(path) == LibraryIndex::Add::Added) ++c.added;
  c.addUs += static_cast<uint64_t>(esp_timer_get_time() - t0);
  const uint32_t f = internalFree();
  if (f < c.minFree) c.minFree = f;
}

}  // namespace

Spike::Spike(Core2AudioBackend& audio, Haptics& haptics, LocalStorage& storage)
    : audio_(audio), haptics_(haptics), storage_(storage) {}

bool Spike::ensureIndex() {
  if (!index_) index_ = psramNew<LibraryIndex>(psramAlloc, psramFree);
  if (!index_) Serial.println("[index] no PSRAM for the index");
  return index_ != nullptr;
}

void Spike::begin() {
  if (!storage_.available()) {
    Serial.println("[index] no storage: no index (g<n> builds a synthetic one)");
    return;
  }
  buildReal();
  report();
}

bool Spike::ownsScreen() const {
  return (input_ && input_->active()) || (scroll_ && scroll_->active()) || (font_ && font_->active()) ||
         (thumb_ && thumb_->active());
}

bool Spike::ownsInput() const { return input_ && input_->active(); }

void Spike::closeAllBut(Screen keep) {
  if (keep != Screen::Input && input_) input_->close();
  if (keep != Screen::Scroll && scroll_) scroll_->close();
  if (keep != Screen::Font && font_) font_->close();
  if (keep != Screen::Thumb && thumb_) thumb_->close();
}

void Spike::loop(uint32_t nowMs) {
  if (input_) input_->loop(nowMs);
  if (scroll_) scroll_->loop(nowMs);
  // The probes' pages are static: a tap on the glass closes them.
  if ((font_ && font_->active()) || (thumb_ && thumb_->active())) {
    for (size_t i = 0; i < M5.Touch.getCount(); ++i) {
      const auto& t = M5.Touch.getDetail(i);
      if (t.wasClicked() && t.y >= 0 && t.y < 240) {
        if (font_) font_->close();
        if (thumb_) thumb_->close();
        break;
      }
    }
  }
  const bool owned = ownsScreen();
  if (owned_ && !owned && released_) released_();
  owned_ = owned;
}

void Spike::inputLab(const char* arg) {
  if (!input_) input_ = psramNew<InputLab>(haptics_);
  if (!input_ || !input_->ready()) {
    Serial.println("[input] no PSRAM for the input lab");
    return;
  }
  if (opens(arg, input_->active())) closeAllBut(Screen::Input);
  input_->command(arg);
}

void Spike::scrollLab(const char* arg) {
  if (!scroll_) scroll_ = psramNew<ScrollLab>(audio_, haptics_);
  if (!scroll_) {
    Serial.println("[scroll] no PSRAM for the scroll lab");
    return;
  }
  if (opens(arg, scroll_->active())) closeAllBut(Screen::Scroll);
  scroll_->command(arg, index_);
}

void Spike::fontProbe(const char* arg) {
  if (!font_) font_ = psramNew<FontProbe>();
  if (!font_) {
    Serial.println("[font] no PSRAM for the font probe");
    return;
  }
  if (opens(arg, font_->active())) closeAllBut(Screen::Font);
  font_->command(arg);
}

void Spike::thumbProbe(const char* arg) {
  if (!thumb_) thumb_ = psramNew<ThumbProbe>(&audio_);
  if (!thumb_) {
    Serial.println("[thumb] no PSRAM for the thumbnail probe");
    return;
  }
  if (opens(arg, thumb_->active())) closeAllBut(Screen::Thumb);
  thumb_->command(arg, storage_.available() ? &storage_.fs() : nullptr, index_);
}

void Spike::index(const char* arg) {
  if (!arg || !*arg) {
    report();
    return;
  }
  const long n = atol(arg);
  if (arg[0] == '0' && n == 0) {
    if (buildReal()) report();
    return;
  }
  if (n < 1 || n > 50000) {
    Serial.println("[index] g: report; g0: rebuild from the SD card; g<n>: synthetic library of n tracks (1-50000)");
    return;
  }
  if (buildSynthetic(static_cast<uint32_t>(n))) report();
}

bool Spike::buildReal() {
  if (!storage_.available() || !ensureIndex()) return false;
  if (scroll_) scroll_->indexChanging();
  index_->clear();
  Build b;
  b.internalFreeBefore = internalFree();
  const uint32_t psBefore = psramFreeNow();
  WalkCtx ctx;
  ctx.index = index_;
  ctx.minFree = b.internalFreeBefore;
  const int64_t t0 = esp_timer_get_time();
  index_->begin("/music");
  b.files = storage_.forEachFile(onFile, &ctx);
  const int64_t t1 = esp_timer_get_time();
  const bool ok = index_->finish();
  const int64_t t2 = esp_timer_get_time();
  b.added = ctx.added;
  b.addMs = ctx.addUs / 1000.0f;
  b.walkMs = (t1 - t0) / 1000.0f - b.addMs;
  b.finishMs = (t2 - t1) / 1000.0f;
  b.psramUsed = static_cast<int32_t>(psBefore) - static_cast<int32_t>(psramFreeNow());
  b.internalDelta = static_cast<int32_t>(internalFree()) - static_cast<int32_t>(b.internalFreeBefore);
  b.internalMinDuring = std::min(ctx.minFree, internalFree());
  build_ = b;
  if (!ok) Serial.println("[index] build FAILED (out of PSRAM)");
  return ok;
}

bool Spike::buildSynthetic(uint32_t tracks) {
  if (!ensureIndex()) return false;
  if (scroll_) scroll_->indexChanging();
  index_->clear();
  const synth::Spec spec = synth::specFor(tracks);
  Build b;
  b.synthetic = true;
  b.internalFreeBefore = internalFree();
  const uint32_t psBefore = psramFreeNow();
  const int64_t t0 = esp_timer_get_time();
  index_->begin(spec.root);  // no size hint: the same growth as a real scan
  b.added = synth::addTracks(*index_, spec);
  b.files = spec.tracks;
  const int64_t t1 = esp_timer_get_time();
  const uint32_t minDuring = internalFree();
  const bool ok = index_->finish();
  const int64_t t2 = esp_timer_get_time();
  b.addMs = (t1 - t0) / 1000.0f;  // path generation included (snprintf of ~70 B a track)
  b.walkMs = 0;
  b.finishMs = (t2 - t1) / 1000.0f;
  b.psramUsed = static_cast<int32_t>(psBefore) - static_cast<int32_t>(psramFreeNow());
  b.internalDelta = static_cast<int32_t>(internalFree()) - static_cast<int32_t>(b.internalFreeBefore);
  b.internalMinDuring = std::min(minDuring, internalFree());
  build_ = b;
  if (!ok) Serial.println("[index] build FAILED (out of PSRAM)");
  Serial.printf("[index] synthetic: %lu tracks, %lu artists, %lu albums asked\n", (unsigned long)spec.tracks,
                (unsigned long)spec.artists, (unsigned long)spec.albums);
  return ok;
}

void Spike::report() {
  if (!index_ || !index_->ready()) {
    Serial.println("[index] empty (g0: from the SD card, g<n>: synthetic)");
  } else {
    const LibraryIndex::Memory m = index_->memory();
    const uint32_t t = index_->trackCount();
    Serial.printf("[index] %s: %lu files seen, %lu tracks, %lu artists, %lu albums, %lu folders\n",
                  build_.synthetic ? "synthetic" : "SD card /music", (unsigned long)build_.files, (unsigned long)t,
                  (unsigned long)index_->artistCount(), (unsigned long)index_->albumCount(),
                  (unsigned long)index_->folderCount());
    if (build_.synthetic) {
      Serial.printf("[index] time: add %.1f ms (making the paths included), finish %.1f ms (sort + views + trim)\n",
                    build_.addMs, build_.finishMs);
    } else {
      Serial.printf("[index] time: walk %.1f ms (the file system), add %.1f ms, finish %.1f ms (sort + views + "
                    "trim)\n",
                    build_.walkMs, build_.addMs, build_.finishMs);
    }
    Serial.printf("[index] PSRAM: %u B held (%.1f B/track): strings %u, tracks %u, artists %u, albums %u, folders %u, "
                  "views %u; build peak %u B; PSRAM free fell %ld B\n",
                  (unsigned)m.total, t ? static_cast<float>(m.total) / t : 0.0f, (unsigned)m.strings,
                  (unsigned)m.tracks, (unsigned)m.artists, (unsigned)m.albums, (unsigned)m.folders, (unsigned)m.views,
                  (unsigned)m.buildPeak, (long)build_.psramUsed);
    Serial.printf("[index] internal RAM: free %lu B before, %+ld B after the build, lowest %lu B during; the index "
                  "object itself is in PSRAM (%u B)\n",
                  (unsigned long)build_.internalFreeBefore, (long)build_.internalDelta,
                  (unsigned long)build_.internalMinDuring, (unsigned)sizeof(LibraryIndex));
    const LibraryIndex::Span a = index_->artistsAZ();
    Serial.print("[index] artists A-Z:");
    for (uint32_t i = 0; i < a.count && i < 4; ++i) Serial.printf(" \"%s\"", index_->artistName(a[i]));
    Serial.printf("%s; rail buckets:", a.count > 4 ? " ..." : "");
    for (int bkt = 0; bkt < LibraryIndex::kBuckets; ++bkt) {
      const uint32_t n = index_->bucketStart(LibraryIndex::View::Artists, bkt + 1) -
                         index_->bucketStart(LibraryIndex::View::Artists, bkt);
      if (n) Serial.printf(" %c%lu", bkt == 0 ? '#' : 'A' + bkt - 1, (unsigned long)n);
    }
    Serial.println();
  }
  if (listTracks_) {
    Serial.printf("[index] today's track list (std::vector<Track>): %lu tracks; internal RAM: `library` measured %ld B "
                  "(counted %lu B), the playlist copy measured %ld B (counted %lu B): %.0f B/track for both (counted: "
                  "the blocks that are really internal; the vectors' own buffers, 4 KB and over, went to PSRAM; the "
                  "measured deltas also catch other tasks' allocations)\n",
                  (unsigned long)listTracks_, (long)listMeasured_, (unsigned long)listEstimate_,
                  (long)playlistMeasured_, (unsigned long)playlistEstimate_,
                  listTracks_ ? static_cast<float>(listEstimate_ + playlistEstimate_) / listTracks_ : 0.0f);
  }
}

void Spike::setTrackListCost(uint32_t tracks, int32_t libraryMeasured, int32_t playlistMeasured,
                             uint32_t libraryEstimate, uint32_t playlistEstimate) {
  listTracks_ = tracks;
  listMeasured_ = libraryMeasured;
  playlistMeasured_ = playlistMeasured;
  listEstimate_ = libraryEstimate;
  playlistEstimate_ = playlistEstimate;
}

uint32_t Spike::estimateBytes(const std::vector<Track>& tracks) {
  // Internal RAM only. ESP-IDF's heap: 4-byte aligned blocks with an 8-byte
  // header (no poisoning); libstdc++ keeps strings of up to 15 bytes inside
  // the std::string itself. A block of 4 KB or more goes to PSRAM
  // (SPIRAM_MALLOC_ALWAYSINTERNAL): with 77 tracks the vectors' own buffers
  // (76 B a Track) are there, and only the strings are internal. So each
  // block is counted by where it really is.
  auto block = [](const void* p, size_t n) -> uint32_t {
    if (!p || esp_ptr_external_ram(p)) return 0;
    return static_cast<uint32_t>(((n + 3) & ~static_cast<size_t>(3)) + 8);
  };
  auto str = [&](const std::string& s) -> uint32_t { return s.capacity() > 15 ? block(s.data(), s.capacity() + 1) : 0; };
  uint32_t total = tracks.capacity() ? block(tracks.data(), tracks.capacity() * sizeof(Track)) : 0;
  for (const Track& t : tracks) total += str(t.path) + str(t.title) + str(t.artist);
  return total;
}
