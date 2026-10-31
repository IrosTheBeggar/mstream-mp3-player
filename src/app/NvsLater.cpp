// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/NvsLater.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cstdio>
#include <cstring>

#include "SaveLater.h"

namespace nvslater {

namespace {

struct Entry {
  const char* ns = nullptr;
  const char* key = nullptr;
  uint16_t value = 0;
  uint8_t width = 1;  // bytes: 1 (putUChar) or 2 (putUShort)
};

constexpr int kMax = 12;  // more than the settings rows there are
Entry queue_[kMax];
int n_ = 0;
SaveLater later_;

void put(const char* ns, const char* key, uint16_t value, uint8_t width) {
  for (int i = 0; i < n_; ++i) {
    if (std::strcmp(queue_[i].ns, ns) == 0 && std::strcmp(queue_[i].key, key) == 0) {
      queue_[i].value = value;
      queue_[i].width = width;
      later_.changed(millis());
      return;
    }
  }
  if (n_ == kMax) flushNow();  // (never, with the keys there are)
  queue_[n_].ns = ns;
  queue_[n_].key = key;
  queue_[n_].value = value;
  queue_[n_].width = width;
  ++n_;
  later_.changed(millis());
}

}  // namespace

void putU8(const char* ns, const char* key, uint8_t value) { put(ns, key, value, 1); }

void putU16(const char* ns, const char* key, uint16_t value) { put(ns, key, value, 2); }

bool pending() { return n_ > 0; }

void loop(uint32_t nowMs) {
  if (later_.due(nowMs)) flushNow();
}

void flushNow() {
  if (n_ == 0) return;
  const uint32_t t0 = millis();
  char what[200] = "";
  size_t len = 0;
  int written = 0, failed = 0;
  bool done[kMax] = {};
  for (int i = 0; i < n_; ++i) {
    if (done[i]) continue;
    // This namespace's keys, under one open.
    Preferences p;
    const bool open = p.begin(queue_[i].ns, false);
    for (int j = i; j < n_; ++j) {
      if (done[j] || std::strcmp(queue_[j].ns, queue_[i].ns) != 0) continue;
      done[j] = true;
      bool ok = false;
      if (open) {
        ok = queue_[j].width == 1 ? p.putUChar(queue_[j].key, static_cast<uint8_t>(queue_[j].value)) == 1
                                  : p.putUShort(queue_[j].key, queue_[j].value) == sizeof(uint16_t);
      }
      if (ok) {
        ++written;
      } else {
        ++failed;
      }
      if (len < sizeof(what)) {
        const int m = std::snprintf(what + len, sizeof(what) - len, "%s%s/%s%s", len ? ", " : "", queue_[j].ns,
                                    queue_[j].key, ok ? "" : " (NOT saved)");
        if (m > 0) len += static_cast<size_t>(m);
      }
    }
    if (open) p.end();
  }
  n_ = 0;
  later_.written();
  const int keys = written + failed;
  Serial.printf("[nvs] saved %s (%d key%s, %lu ms)%s\n", what, keys, keys == 1 ? "" : "s",
                static_cast<unsigned long>(millis() - t0), failed ? ": some NOT saved, for this boot only" : "");
}

}  // namespace nvslater
