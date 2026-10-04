// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "PairFinds.h"

#include <cstdio>
#include <cstring>

#include "OutputModel.h"

namespace {

void copyName(char* dst, size_t size, const char* src) {
  size_t i = 0;
  if (src) {
    for (; i + 1 < size && src[i]; ++i) dst[i] = src[i];
  }
  dst[i] = 0;
}

// Appends `text` to buf (`len` of `size` used) if it fits with `reserve`
// bytes to spare; else nothing, false.
bool append(char* buf, size_t size, size_t& len, const char* text, size_t reserve = 0) {
  const size_t n = std::strlen(text);
  if (len + n + reserve + 1 > size) return false;
  std::memcpy(buf + len, text, n + 1);
  len += n;
  return true;
}

}  // namespace

// ---- PairFindRing ----

bool PairFindRing::push(const PairFind& f) {
  if (n_ == kSize) {
    ++dropped_;
    return false;
  }
  d_[(head_ + n_) % kSize] = f;
  ++n_;
  return true;
}

bool PairFindRing::pop(PairFind& out) {
  if (n_ == 0) return false;
  out = d_[head_];
  head_ = (head_ + 1) % kSize;
  --n_;
  return true;
}

// ---- PairFinds ----

void PairFinds::start(uint32_t nowMs) {
  n_ = 0;
  told_ = 0;
  audio_ = 0;
  results_ = 0;
  untold_ = untoldAudio_ = 0;
  startMs_ = nowMs;
}

PairFinds::Say PairFinds::note(const PairFind& f) {
  // A name alone (cod 0: Bluedroid's remote name request, once the inquiry
  // round ends) is about a device an inquiry result showed: no class, no
  // RSSI, nothing to count.
  const bool nameAlone = f.cod == 0;
  if (!nameAlone) ++results_;
  for (int i = 0; i < n_; ++i) {
    PairFind& s = seen_[i];
    if (std::memcmp(s.addr, f.addr, sizeof(s.addr)) != 0) continue;
    if (nameAlone) {
      // The usual way a nameless one's name comes (its later inquiry
      // results carry none either). Kept for a non-audio one too, quietly.
      if (!f.name[0] || s.name[0]) return Say::Nothing;
      copyName(s.name, sizeof(s.name), f.name);
      s.remembered = s.remembered || f.remembered;
      s.linked = s.linked || f.linked;
      if (!s.audio) return Say::Nothing;
      told_ = i;
      return Say::Named;
    }
    if (!f.audio) return Say::Nothing;  // (its class doesn't say audio: nothing new)
    if (!s.audio) {
      // Seen before with a class that didn't say audio: it is one now.
      s.audio = true;
      s.rssi = f.rssi;
      s.cod = f.cod;
      s.remembered = s.remembered || f.remembered;
      s.linked = s.linked || f.linked;
      if (f.name[0]) copyName(s.name, sizeof(s.name), f.name);
      ++audio_;
      told_ = i;
      return Say::Found;
    }
    s.remembered = s.remembered || f.remembered;
    s.linked = s.linked || f.linked;
    if (!s.name[0] && f.name[0]) {
      copyName(s.name, sizeof(s.name), f.name);
      told_ = i;
      return Say::Named;
    }
    return Say::Nothing;  // a repeat (a later round, a new signal or name: not news)
  }
  if (nameAlone) return Say::Nothing;  // (its inquiry result: before this search, or lost on the way)
  if (n_ == kDevices) {
    ++untold_;
    if (f.audio) ++untoldAudio_;
    return Say::Nothing;
  }
  PairFind& s = seen_[n_];
  s = f;
  s.search = 0;
  ++n_;
  if (!f.audio) return Say::Nothing;
  ++audio_;
  told_ = n_ - 1;
  return Say::Found;
}

void PairFinds::describe(const PairFind& f, Say say, char* buf, size_t size) {
  if (!buf || size == 0) return;
  char name[40];
  if (f.name[0]) {
    snprintf(name, sizeof(name), "\"%.31s\"", f.name);
  } else {
    snprintf(name, sizeof(name), "(no name)");
  }
  char rssi[16];
  if (f.rssi <= -127) {
    snprintf(rssi, sizeof(rssi), "no rssi");
  } else {
    snprintf(rssi, sizeof(rssi), "rssi %d", static_cast<int>(f.rssi));
  }
  const char* who = f.remembered && f.linked ? ", the remembered headphones, linked now: not listed"
                    : f.remembered           ? ", the remembered headphones"
                    : f.linked               ? ", linked now: not listed"
                                             : "";
  snprintf(buf, size, "%s%s (%s, class 0x%06lx), %s%s",
           say == Say::Named ? "the one found with no name is " : "found ", name,
           BtScanList::kindName(BtScanList::kindOf(f.cod)), static_cast<unsigned long>(f.cod & 0xFFFFFF), rssi, who);
}

void PairFinds::summary(uint32_t nowMs, uint32_t lost, char* buf, size_t size) const {
  if (!buf || size == 0) return;
  const unsigned long secs = static_cast<unsigned long>((nowMs - startMs_ + 500) / 1000);
  // After the names: the results, and what wasn't told apart or was lost.
  char tail[160];
  size_t tl = 0;
  tail[0] = 0;
  char part[80];
  if (results_) {
    snprintf(part, sizeof(part), "; %lu inquiry result%s", static_cast<unsigned long>(results_),
             results_ == 1 ? "" : "s");
    append(tail, sizeof(tail), tl, part);
  }
  if (untold_) {
    snprintf(part, sizeof(part), "; %lu from devices past the first %d (not told apart; %lu of those audio)",
             static_cast<unsigned long>(untold_), kDevices, static_cast<unsigned long>(untoldAudio_));
    append(tail, sizeof(tail), tl, part);
  }
  if (lost) {
    snprintf(part, sizeof(part), "; %lu more lost on the way to the log", static_cast<unsigned long>(lost));
    append(tail, sizeof(tail), tl, part);
  }
  char head[96];
  if (results_ == 0) {
    snprintf(head, sizeof(head), "the search saw nothing in %lu s (not one inquiry result)", secs);
  } else {
    // Past kDevices it can't count devices, only say there were more.
    char many[24], of[24];
    if (untold_) {
      snprintf(many, sizeof(many), "more than %d devices", n_);
      snprintf(of, sizeof(of), "of the first %d", n_);
    } else {
      snprintf(many, sizeof(many), "%d device%s", n_, n_ == 1 ? "" : "s");
      snprintf(of, sizeof(of), "of them");
    }
    const bool one = n_ == 1 && !untold_;
    if (audio_ == 0) {
      snprintf(head, sizeof(head), "the search saw %s in %lu s, %s%s%s", many, secs, one ? "not an audio one" : "none ",
               one ? "" : of, one ? "" : " audio");
    } else if (one) {
      snprintf(head, sizeof(head), "the search saw %s in %lu s, an audio one: ", many, secs);
    } else {
      snprintf(head, sizeof(head), "the search saw %s in %lu s, %d %s audio: ", many, secs, audio_, of);
    }
  }
  size_t len = 0;
  buf[0] = 0;
  if (!append(buf, size, len, head)) {
    snprintf(buf, size, "%s", head);  // (a tiny buffer: cut)
    return;
  }
  // The audio devices in the order found, as far as they fit with the tail.
  bool first = true;
  for (int i = 0; i < n_; ++i) {
    const PairFind& s = seen_[i];
    if (!s.audio) continue;
    char item[64];
    snprintf(item, sizeof(item), "%s%s%.31s%s%s", first ? "" : ", ", s.name[0] ? "\"" : "",
             s.name[0] ? s.name : "(no name)", s.name[0] ? "\"" : "",
             s.remembered && s.linked ? " (remembered, linked)"
             : s.remembered           ? " (remembered)"
             : s.linked               ? " (linked)"
                                      : "");
    if (!append(buf, size, len, item, tl + 5)) {
      append(buf, size, len, first ? "..." : ", ...");
      break;
    }
    first = false;
  }
  append(buf, size, len, tail);
}
