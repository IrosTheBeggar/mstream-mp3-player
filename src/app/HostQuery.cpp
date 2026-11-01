// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/HostQuery.h"

#include <cstring>

#include "storage/CardSpace.h"

namespace {

bool fat(hoststatus::Card c) { return c == hoststatus::Card::Fat32 || c == hoststatus::Card::Fat16; }

// countBlocked()'s detail, for the log.
const char* blockedText(const char* detail) {
  if (std::strcmp(detail, "playing") == 0) return "music started";
  if (std::strcmp(detail, "library") == 0) return "the library update began";
  return detail;
}

}  // namespace

bool HostQuery::counting() const { return cardspace::counting(); }

void HostQuery::status() {
  hoststatus::Facts f;
  if (hooks_.facts) hooks_.facts(f);
  f.counting = cardspace::counting();
  if (fat(f.card) && !f.counting) f.freeKnown = cardspace::freeBytes(&f.freeBytes);
  char line[hoststatus::kMaxLine + 1];
  hoststatus::format(f, line, sizeof(line));
  Serial.printf("%s\n", line);
}

const char* HostQuery::count(uint32_t nowMs) {
  if (cardspace::counting()) return "counting";
  if (const char* why = hooks_.countBlocked ? hooks_.countBlocked() : nullptr) return why;
  if (!fat(cardspace::card())) return "card";
  if (!cardspace::startCount(nowMs)) {
    Serial.printf("[host] count: can't start (%s)\n", cardspace::failure());
    return "card";
  }
  progress_.start(nowMs);
  Serial.printf("@count 0\n");
  Serial.printf("[host] count: the card's free clusters, asked by a computer (%s, clusters of %lu B)\n",
                hoststatus::cardName(cardspace::card()), static_cast<unsigned long>(cardspace::clusterBytes()));
  return nullptr;
}

void HostQuery::stop(const char* detail) {
  cardspace::stopCount();
  Serial.printf("@err 4 count %s\n", detail);
  Serial.printf("[host] count: stopped at %u%% (%s); nothing changed\n", static_cast<unsigned>(progress_.last()),
                blockedText(detail));
}

void HostQuery::loop(uint32_t nowMs) {
  if (!cardspace::counting()) return;
  if (const char* why = hooks_.countBlocked ? hooks_.countBlocked() : nullptr) {
    stop(why);
    return;
  }
  switch (cardspace::stepCount()) {
    case cardspace::Step::Working:
      if (progress_.due(cardspace::countPercent(), nowMs)) {
        Serial.printf("@count %u\n", static_cast<unsigned>(progress_.last()));
      }
      return;
    case cardspace::Step::Failed:
      Serial.printf("@err 4 count card\n");
      Serial.printf("[host] count: failed at %u%% (%s); nothing changed\n", static_cast<unsigned>(progress_.last()),
                    cardspace::failure());
      return;
    case cardspace::Step::Done: {
      const cardspace::Counted& c = cardspace::counted();
      if (progress_.due(100, nowMs)) Serial.printf("@count 100\n");
      Serial.printf("@count done free=%llu\n", static_cast<unsigned long long>(c.freeBytes));
      Serial.printf("[host] count: %llu B free (%lu clusters of %lu B) in %.1f s; %lu pieces of the FAT's %lu B, %lu "
                    "read (%lu again after a write); %s\n",
                    static_cast<unsigned long long>(c.freeBytes), static_cast<unsigned long>(c.freeClusters),
                    static_cast<unsigned long>(c.clusterBytes),
                    c.ms / 1000.0f, static_cast<unsigned long>(c.pieces), static_cast<unsigned long>(c.fatBytes),
                    static_cast<unsigned long>(c.reads), static_cast<unsigned long>(c.rereads),
                    c.fsinfo ? "FatFs's count now; FSINFO written at its next sync"
                             : "FatFs's count now (no FSINFO sector: counted again after a restart)");
      return;
    }
  }
}

const char* HostQuery::identify(const char* label) {
  const char* why = hooks_.identify ? hooks_.identify(label) : "ui";
  if (why) return why;
  Serial.printf("@identify ok\n");
  return nullptr;
}
