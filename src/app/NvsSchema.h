// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once

// The NVS schema number, "meta"/"schema" (nvslayout; docs/ARCHITECTURE.md,
// "NVS: the rules"): checked at every boot before anything else reads NVS,
// migrated when older, written when absent. The other namespaces' keys
// (the touch calibration, the Bluetooth pairing and bond, the settings,
// the queue) are never touched here unless a migration step says so, and
// schema 1 has none.
namespace nvsschema {

// setup(), right after NVS is up (ensureNvs()) and before the first other
// read (the boot clock's). Serial may not be up yet: log() says what it
// did, later.
void check();
void log();

}  // namespace nvsschema
