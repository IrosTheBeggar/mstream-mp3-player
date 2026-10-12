// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/UiLang.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cstdio>
#include <cstring>

#include "NvsLayout.h"

namespace uilang {
namespace {

bool have = false;     // the key is there
bool unknown = false;  // ... with a tag this firmware hasn't
char stored[nvslayout::kLangMaxBytes] = "";

}  // namespace

void begin() {
  Preferences p;
  // (Read-only: a fresh NVS has no "ui" yet, and begin() fails: none saved.)
  if (p.begin(nvslayout::kUiNamespace, true)) {
    have = p.isKey(nvslayout::kLangKey);
    if (have) p.getString(nvslayout::kLangKey, stored, sizeof(stored));
    p.end();
  }
  const uistr::Lang l = nvslayout::langFrom(have, stored);
  unknown = have && std::strcmp(uistr::tag(l), stored) != 0;
  uistr::setLang(l);
}

void log() {
  const uistr::Lang l = uistr::lang();
  if (unknown) {
    Serial.printf("[ui] language: \"%s\" saved, which this firmware hasn't: English\n", stored);
  } else if (!have) {
    Serial.println("[ui] language: none saved yet: English");
  } else {
    Serial.printf("[ui] language: %s (%s)%s\n", uistr::tag(l), uistr::autonym(l),
                  uistr::hasCatalogue(l) ? "" : ", no catalogue yet: its texts are English's");
  }
}

bool chosen() { return have; }

void choose(uistr::Lang l) {
  uistr::setLang(l);
  const char* t = uistr::tag(uistr::lang());
  Preferences p;
  bool saved = false;
  if (p.begin(nvslayout::kUiNamespace, false)) {
    saved = p.putString(nvslayout::kLangKey, t) == std::strlen(t);
    p.end();
  }
  if (saved) {
    have = true;
    unknown = false;
    snprintf(stored, sizeof(stored), "%s", t);
  }
  Serial.printf("[ui] language: %s (%s), %s\n", t, uistr::autonym(uistr::lang()),
                saved ? "saved" : "NOT saved (NVS): for this boot only");
}

void forget() {
  Preferences p;
  if (p.begin(nvslayout::kUiNamespace, false)) {
    p.remove(nvslayout::kLangKey);
    p.remove(nvslayout::kCoachKey);
    p.end();
  }
  have = unknown = false;
  stored[0] = 0;
  uistr::setLang(uistr::Lang::En);
  Serial.println("[ui] language and tips forgotten: English; the next boot asks for a language, then shows the tips");
}

}  // namespace uilang
