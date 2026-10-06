// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/NvsSchema.h"

#include <Arduino.h>
#include <Preferences.h>

#include "NvsLayout.h"

namespace nvsschema {
namespace {

using Action = nvslayout::SchemaStep::Action;

nvslayout::SchemaStep step;
bool opened = false;
bool migrated = true;
bool written = false;

// One step per schema bump, from `from` up to `to` (each step the next
// number's changes: a key moved, a blob rewritten). Schema 1 is the first.
// The step from 1 to 2 is nothing: schema 2 only adds "queue"/"repeat",
// and an absent key reads as Off (QueueStore::loadRepeat()). False: a step
// failed; the number then stays where it was, so the next boot tries again.
bool migrate(uint16_t from, uint16_t to) {
  (void)from;
  (void)to;
  return true;
}

}  // namespace

void check() {
  Preferences p;
  opened = p.begin(nvslayout::kMetaNamespace, false);
  if (!opened) return;  // (NVS failed: logNvs() says so)
  const bool have = p.isKey(nvslayout::kSchemaKey);
  step = nvslayout::schemaStep(have, have ? p.getUShort(nvslayout::kSchemaKey, 0) : 0);
  if (step.action == Action::Migrate) migrated = migrate(step.from, nvslayout::kCurrent);
  if ((step.action == Action::Write || step.action == Action::Migrate) && migrated) {
    written = p.putUShort(nvslayout::kSchemaKey, nvslayout::kCurrent) == sizeof(uint16_t);
  }
  p.end();
}

void log() {
  if (!opened) {
    Serial.println("[nvs] schema: \"meta\" can't be opened: not checked");
    return;
  }
  const unsigned cur = nvslayout::kCurrent, from = step.from;
  switch (step.action) {
    case Action::None:
      Serial.printf("[nvs] schema %u\n", cur);
      break;
    case Action::Write:
      Serial.printf("[nvs] schema: none yet (a fresh NVS, or v0.5.0-beta.1's): %u %s\n", cur,
                    written ? "written" : "NOT written");
      break;
    case Action::Migrate:
      Serial.printf("[nvs] schema %u -> %u: %s\n", from, cur,
                    !migrated ? "a migration step FAILED (tried again at the next boot)"
                    : written ? "migrated"
                              : "migrated, the number NOT written");
      break;
    case Action::Newer:
      Serial.printf("[nvs] schema %u, newer than this firmware's %u (a downgrade): left as it is; "
                    "settings it doesn't know are ignored\n",
                    from, cur);
      break;
  }
}

}  // namespace nvsschema
