#include "TrackCatalog.h"

#include <cstring>

namespace {

struct Builtin {
  const char* path;
  const char* title;
  uint32_t durationMs;
};

// The backend makes these itself (ToneGen, ClickGen): no file behind them.
constexpr Builtin kBuiltins[] = {
    {"tone:440", "Test tone 440 Hz", 0},
    {"tone:1000", "Test tone 1 kHz", 0},
    {"tone:left", "Left ear only", 0},
    // Click tracks with a known beat, for the dancer's beat tracker.
    {"tone:click90", "Clicks 90 BPM", 60000},
    {"tone:click120", "Clicks 120 BPM", 60000},
    {"tone:click128", "Clicks 128 BPM", 60000},
    {"tone:click140", "Clicks 140 BPM", 60000},
    {"tone:click174", "Clicks 174 BPM", 60000},
    {"tone:click120off", "Clicks 120 BPM, late start", 60000},
};
constexpr uint32_t kCount = sizeof(kBuiltins) / sizeof(kBuiltins[0]);

constexpr uint32_t B = TrackCatalog::kBuiltin;
constexpr uint32_t kIds[] = {B + 0, B + 1, B + 2, B + 3, B + 4, B + 5, B + 6, B + 7, B + 8};
static_assert(sizeof(kIds) / sizeof(kIds[0]) == kCount, "one id per built-in track");

// Copies `len` bytes and a NUL, or writes "" and returns 0 if it won't fit.
size_t copyOut(const char* s, size_t len, char* buf, size_t size) {
  if (size == 0) return 0;
  if (len + 1 > size) {
    buf[0] = 0;
    return 0;
  }
  std::memcpy(buf, s, len);
  buf[len] = 0;
  return len;
}

// Copies what fits of `len` bytes, cut at a UTF-8 code point boundary.
size_t copyCut(const char* s, size_t len, char* buf, size_t size) {
  if (size == 0) return 0;
  if (len + 1 > size) {
    len = size - 1;
    while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) --len;  // not mid-character
  }
  return copyOut(s, len, buf, size);
}

}  // namespace

uint32_t TrackCatalog::builtinCount() { return kCount; }

LibraryIndex::Span TrackCatalog::builtins() { return {kIds, kCount}; }

bool TrackCatalog::valid(uint32_t id) const { return isBuiltin(id) || inIndex(id); }

size_t TrackCatalog::path(uint32_t id, char* buf, size_t size) const {
  if (isBuiltin(id)) {
    const char* p = kBuiltins[id - kBuiltin].path;
    return copyOut(p, std::strlen(p), buf, size);
  }
  if (inIndex(id)) return index_->trackPath(id, buf, size);
  return copyOut("", 0, buf, size);
}

size_t TrackCatalog::title(uint32_t id, char* buf, size_t size) const {
  if (isBuiltin(id)) {
    const char* t = kBuiltins[id - kBuiltin].title;
    return copyCut(t, std::strlen(t), buf, size);
  }
  if (inIndex(id)) {
    uint8_t len = 0;
    const char* t = index_->trackTitle(id, &len);
    return copyCut(t, len, buf, size);
  }
  return copyOut("", 0, buf, size);
}

const char* TrackCatalog::artist(uint32_t id) const {
  if (isBuiltin(id)) return "built-in";
  return inIndex(id) ? index_->artistName(index_->track(id).artist) : "";
}

const char* TrackCatalog::album(uint32_t id) const {
  if (isBuiltin(id)) return "Built-in";
  return inIndex(id) ? index_->albumName(index_->track(id).album) : "";
}

uint32_t TrackCatalog::durationHintMs(uint32_t id) const {
  return isBuiltin(id) ? kBuiltins[id - kBuiltin].durationMs : 0;
}

uint32_t TrackCatalog::find(const char* path) const {
  if (!path) return kNone;
  if (std::strncmp(path, "tone:", 5) == 0) {
    for (uint32_t i = 0; i < kCount; ++i) {
      if (std::strcmp(kBuiltins[i].path, path) == 0) return kBuiltin + i;
    }
    return kNone;
  }
  return index_ ? index_->findTrack(path) : kNone;
}
