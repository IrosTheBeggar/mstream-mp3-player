// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once

#include "UiStrings.h"

// The UI's language (docs/I18N.md, phase 3): NVS "ui"/"lang", a BCP-47 tag
// (nvslayout, schema 3), read once at boot into uistr::setLang() before
// anything draws a text (the boot screen's touch hint is the first), and
// written at once when the listener picks one (Output > Language, or the
// first boot's picker: ui/Ui). No key, or a tag this firmware hasn't (a
// later one's, after a downgrade): English, as every firmware before.
namespace uilang {

// setup(), after nvsschema::check() (whose step from schema 2 writes "en"
// on an update) and before the boot screen. Serial may not be up yet:
// log() says what it read, later.
void begin();
void log();
// A language was saved (the first boot's picker shows only when not:
// nvslayout::langPickerDue()).
bool chosen();
// The UI's language from now on, saved (a failed write keeps it for this
// boot and says so). The caller draws everything again (Ui).
void choose(uistr::Lang l);
// The language and the first-boot tips forgotten (the console's uiL-): the
// next boot is a fresh unit's, its picker and then the tips. English now.
void forget();

}  // namespace uilang
