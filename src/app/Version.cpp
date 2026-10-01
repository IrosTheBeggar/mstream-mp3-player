// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/Version.h"

#include <esp_app_desc.h>
#include <sdkconfig.h>

#include <cstdio>

#include "PlayerVersion.h"  // generated: tools/version.py

// The image's app description, with our version in it. ESP-IDF's own (in
// the prebuilt libesp_app_format.a) is weak and carries the Arduino libs'
// build ("6671d0b", "arduino-lib-builder"), since pioarduino has no
// PROJECT_VER; this one replaces it. The other fields are as IDF's
// esp_app_desc.c fills them for this sdkconfig. The linker script puts it
// first in the flash rodata, where the bootloader, esptool and OTA read it,
// and esptool writes the ELF's SHA-256 into app_elf_sha256 (pioarduino's
// --elf-sha256-offset 0xb0). tools/version.py checks the built image.
// The time and date are the commit's, not the build's.
extern "C" {
extern const esp_app_desc_t esp_app_desc;
const __attribute__((section(".rodata_desc"), used)) esp_app_desc_t esp_app_desc = {
    .magic_word = ESP_APP_DESC_MAGIC_WORD,
#ifdef CONFIG_BOOTLOADER_APP_SECURE_VERSION
    .secure_version = CONFIG_BOOTLOADER_APP_SECURE_VERSION,
#else
    .secure_version = 0,
#endif
    .reserv1 = {},
    .version = PLAYER_VERSION_DESC,
    .project_name = "mstream-mp3-player",
    .time = PLAYER_COMMIT_TIME,
    .date = PLAYER_COMMIT_DATE,
    .idf_ver = IDF_VER,
    .app_elf_sha256 = {},
    .min_efuse_blk_rev_full = CONFIG_ESP_EFUSE_BLOCK_REV_MIN_FULL,
    .max_efuse_blk_rev_full = CONFIG_ESP_EFUSE_BLOCK_REV_MAX_FULL,
    .mmu_page_size = 31 - __builtin_clz(CONFIG_MMU_PAGE_SIZE),  // log2
    .reserv3 = {},
    .reserv2 = {},
};
}

namespace version {

const char* player() { return PLAYER_VERSION; }

const char* commit() {
  static char shortHash[8] = "";
  if (!shortHash[0]) snprintf(shortHash, sizeof(shortHash), "%s", PLAYER_COMMIT);
  return shortHash;
}

const char* commitDate() { return PLAYER_COMMIT_DATE; }

const char* elfSha() {
  static char sha[9] = "";
  if (!sha[0]) esp_app_get_elf_sha256(sha, sizeof(sha));
  return sha;
}

const char* appDesc() { return esp_app_get_description()->version; }

}  // namespace version
