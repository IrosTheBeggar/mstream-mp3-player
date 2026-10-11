// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "DeviceInfo.h"

#include <cstdio>
#include <cstring>

#include "ChipRevision.h"
#include "LibraryText.h"
#include "UiStrings.h"

namespace deviceinfo {

using uistr::S;

namespace {
unsigned long kb(uint32_t bytes) { return static_cast<unsigned long>(bytes / 1024); }

// The storage's word in the Library row: "SD" as it is; the flash and none
// in the UI's language (LocalStorage::name() stays the log's word).
const char* storageShown(const char* name) {
  if (std::strcmp(name, "flash") == 0) return tr(S::device_info_storage_flash);
  if (std::strcmp(name, "none") == 0) return tr(S::device_info_storage_none);
  return name;
}
}  // namespace

const char* label(Item i) {
  switch (i) {
    case Item::Board: return tr(S::device_info_board);
    case Item::PowerChip: return tr(S::device_info_power_chip);
    case Item::Imu: return tr(S::device_info_imu);
    case Item::Chip: return tr(S::device_info_chip);
    case Item::Cpu: return tr(S::device_info_cpu);
    case Item::Flash: return tr(S::device_info_flash);
    case Item::Psram: return tr(S::device_info_psram);
    case Item::LastReset: return tr(S::device_info_last_reset);
    case Item::Battery: return tr(S::device_info_battery);
    case Item::Library: return tr(S::device_info_library);
    case Item::RamFree: return tr(S::device_info_ram_free);
    case Item::Uptime: return tr(S::device_info_uptime);
    case Item::Firmware: return tr(S::device_info_firmware);
    case Item::Build: return tr(S::device_info_build);
  }
  return "";
}

bool changes(Item i) {
  switch (i) {
    case Item::Cpu:  // the console's Pc80 lowers the clock for a while
    case Item::Psram:
    case Item::Battery:
    case Item::Library:  // "Try again" with no music walks the card again; the scan's updates
    case Item::RamFree:
    case Item::Uptime: return true;
    default: return false;
  }
}

void uptimeText(uint32_t s, char* out, size_t size) {
  using uistr::Arg;
  const unsigned long m = s / 60, h = s / 3600, d = s / 86400;
  if (s < 60) {
    trf(out, size, S::format_seconds, s);
  } else if (s < 3600) {
    trf(out, size, S::format_minutes_seconds, m, Arg::digits(s % 60, 2));
  } else if (s < 86400) {
    trf(out, size, S::format_hours_minutes, h, Arg::digits(m % 60, 2));
  } else {
    trf(out, size, S::format_days_hours, d, h % 24);
  }
}

const char* value(Item i, const Facts& f, char* out, size_t size) {
  if (!size) return out;
  out[0] = 0;
  switch (i) {
    case Item::Board: snprintf(out, size, "%s", f.board); break;
    case Item::PowerChip: snprintf(out, size, "%s", f.pmic); break;
    case Item::Imu: snprintf(out, size, "%s", f.imu); break;
    case Item::Chip: {
      char rev[8];
      chiprev::text(f.chipRevision, rev, sizeof(rev));
      trf(out, size, S::device_info_chip_rev, f.chip, rev);
      break;
    }
    case Item::Cpu: trf(out, size, S::format_mhz, f.cpuMhz); break;
    case Item::Flash: trf(out, size, S::format_mb, f.flashBytes / (1024ul * 1024ul)); break;
    case Item::Psram: trf(out, size, S::device_info_psram_value, kb(f.psramBytes), kb(f.psramFree)); break;
    case Item::LastReset: snprintf(out, size, "%s", f.lastReset); break;
    case Item::Battery: {
      // "78%", "78%, 4.15 V" (and ", charging" after either); "not known".
      char level[40];
      if (f.battery < 0) {
        trf(level, sizeof(level), S::device_info_battery_unknown);
      } else if (f.batteryMv) {
        char pct[16];
        trf(pct, sizeof(pct), S::format_percent, f.battery);
        const unsigned cv = (f.batteryMv + 5u) / 10u;  // centivolts, rounded
        trf(level, sizeof(level), S::device_info_battery_volts, pct, uistr::fixed(cv, 2));
      } else {
        trf(level, sizeof(level), S::format_percent, f.battery);
      }
      if (f.charging) {
        trf(out, size, S::about_battery_charging, level);
      } else {
        uistr::format(out, size, "{0}", level);
      }
      break;
    }
    case Item::Library: {
      // "SD, 19,410 tracks, 99% tagged": the count grouped as the Output
      // tab's Library row has it, and where the names come from in that
      // row's shortest form ("names from the files" with no tags read);
      // "updating..." behind the update's fence.
      char tracks[48];
      trnf(tracks, sizeof(tracks), S::library_tracks_n, f.tracks, uistr::grouped(f.tracks));
      const char* storage = storageShown(f.storage);
      librarytext::Sources src;
      src.transfer = f.fromTransfer;
      src.device = f.fromDevice;
      src.none = f.fromNone;
      if (f.tracks && f.updating) {
        trf(out, size, S::device_info_library_more, storage, tracks, tr(S::library_row_updating));
      } else if (f.tracks && src.total()) {
        char names[64];
        librarytext::sourcesText(src, librarytext::kSourceForms - 1, names, sizeof(names));
        trf(out, size, S::device_info_library_more, storage, tracks, names);
      } else {
        trf(out, size, S::device_info_library_value, storage, tracks);
      }
      break;
    }
    case Item::RamFree:
      trf(out, size, S::device_info_ram_value, kb(f.ramFree), kb(f.ramMin), kb(f.ramBlock));
      break;
    case Item::Uptime: uptimeText(f.uptimeS, out, size); break;
    case Item::Firmware: snprintf(out, size, "%s", f.version); break;
    case Item::Build:
      if (f.commit[0]) {
        snprintf(out, size, "%s, %s, ELF %s", f.commit, f.built, f.elf);
      } else {
        snprintf(out, size, "%s, ELF %s", f.built, f.elf);
      }
      break;
  }
  return out;
}

}  // namespace deviceinfo
