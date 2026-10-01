// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once

// The firmware's version and this build's identity. tools/version.py writes
// them from git into the build directory (PlayerVersion.h, included by
// Version.cpp alone, so a new commit recompiles one file); Version.cpp also
// puts the version in the image's app description (esp_app_desc), which a
// future OTA update compares.
namespace version {

// "v0.5.0" (a release, built from its tag), "v0.5.0-3-gabc1234-dirty" (3
// commits past it, with uncommitted changes), "v0.5.0-dev+abc1234" (no v*
// tag yet: the next release's dev build).
const char* player();
// The commit, its first 7 hex digits ("" without git).
const char* commit();
// The commit's date, UTC: "2026-09-30" (a build's date that doesn't change
// when the same commit is built again).
const char* commitDate();
// The first 8 hex digits of the ELF's SHA-256 (esp_app_get_elf_sha256(),
// written into the image by esptool): matches a crash report to the
// firmware.elf that decodes it.
const char* elfSha();
// The app description's version (esp_app_get_description()): the same as
// player(), cut to its 31 characters.
const char* appDesc();

}  // namespace version
