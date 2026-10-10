// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "DeviceInfo.h"

#include <cstdio>

#include "ChipRevision.h"
#include "LibraryText.h"
#include "QueueView.h"
#include "UiText.h"

namespace deviceinfo {

namespace {
unsigned long kb(uint32_t bytes) { return static_cast<unsigned long>(bytes / 1024); }
}  // namespace

const char* label(Item i) {
  switch (i) {
    case Item::Board: return "Board";
    case Item::PowerChip: return "Power chip";
    case Item::Imu: return "IMU";
    case Item::Chip: return "Chip";
    case Item::Cpu: return "CPU";
    case Item::Flash: return "Flash";
    case Item::Psram: return "PSRAM";
    case Item::LastReset: return "Last reset";
    case Item::Battery: return "Battery";
    case Item::Library: return "Library";
    case Item::RamFree: return "RAM free";
    case Item::Uptime: return "Uptime";
    case Item::Firmware: return "Firmware";
    case Item::Build: return "Build";
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
  const unsigned long m = s / 60, h = s / 3600, d = s / 86400;
  if (s < 60) {
    snprintf(out, size, "%lu s", static_cast<unsigned long>(s));
  } else if (s < 3600) {
    snprintf(out, size, "%lu min %02lu s", m, static_cast<unsigned long>(s % 60));
  } else if (s < 86400) {
    snprintf(out, size, "%lu h %02lu min", h, m % 60);
  } else {
    snprintf(out, size, "%lu d %lu h", d, h % 24);
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
      snprintf(out, size, "%s rev %s", f.chip, rev);
      break;
    }
    case Item::Cpu: snprintf(out, size, "%u MHz", static_cast<unsigned>(f.cpuMhz)); break;
    case Item::Flash:
      snprintf(out, size, "%lu MB", static_cast<unsigned long>(f.flashBytes / (1024ul * 1024ul)));
      break;
    case Item::Psram: snprintf(out, size, "%luK (%luK free)", kb(f.psramBytes), kb(f.psramFree)); break;
    case Item::LastReset: snprintf(out, size, "%s", f.lastReset); break;
    case Item::Battery: {
      if (f.battery < 0) {
        snprintf(out, size, "not known%s", f.charging ? ", charging" : "");
        break;
      }
      int n = snprintf(out, size, "%d%%", f.battery);
      if (f.batteryMv && n > 0 && static_cast<size_t>(n) < size) {
        const unsigned cv = (f.batteryMv + 5u) / 10u;  // centivolts, rounded
        n += snprintf(out + n, size - n, ", %u.%02u V", cv / 100u, cv % 100u);
      }
      if (f.charging && n > 0 && static_cast<size_t>(n) < size) snprintf(out + n, size - n, ", charging");
      break;
    }
    case Item::Library: {
      // "SD, 19,410 tracks, 99% tagged": the count grouped as the Output
      // tab's Library row has it, and where the names come from in that
      // row's shortest form ("names from the files" with no tags read);
      // "updating..." behind the update's fence.
      char n[16];
      queueview::grouped(f.tracks, n, sizeof(n));
      const int at = snprintf(out, size, "%s, %s %s", f.storage, n, f.tracks == 1 ? "track" : "tracks");
      if (!f.tracks || at <= 0 || static_cast<size_t>(at) >= size) break;
      librarytext::Sources src;
      src.transfer = f.fromTransfer;
      src.device = f.fromDevice;
      src.none = f.fromNone;
      if (f.updating) {
        snprintf(out + at, size - at, ", %s", uitext::kLibraryRowUpdating);
      } else if (src.total()) {
        char names[32];
        librarytext::sourcesText(src, librarytext::kSourceForms - 1, names, sizeof(names));
        snprintf(out + at, size - at, ", %s", names);
      }
      break;
    }
    case Item::RamFree:
      snprintf(out, size, "%luK (min %luK, block %luK)", kb(f.ramFree), kb(f.ramMin), kb(f.ramBlock));
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
