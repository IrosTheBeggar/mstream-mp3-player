// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "PowerChoices.h"

#include <cstdio>

#include "UiStrings.h"

namespace powerchoice {

namespace {
using uistr::S;
constexpr TxLevels kBtLevels[kBtChoices] = {{0, 2}, {0, 5}, {0, 7}};  // -12..-6, -12..+3, -12..+9 dBm
constexpr S kBtLabel[kBtChoices] = {S::settings_bt_power_low, S::settings_bt_power_normal, S::settings_bt_power_high};
constexpr S kBtSub[kBtChoices] = {S::settings_bt_low_sub, S::settings_bt_normal_sub, S::settings_bt_high_sub};

int clampBt(int c) { return c < 0 || c >= kBtChoices ? kDefaultBt : c; }
}  // namespace

// ---- CPU speed ----

bool validCpuMhz(uint32_t mhz) { return mhz == 160 || mhz == 240; }

uint16_t cpuMhzFromStored(uint32_t stored) {
  return validCpuMhz(stored) ? static_cast<uint16_t>(stored) : kDefaultCpuMhz;
}

const char* cpuLabel(uint16_t mhz) {
  return tr(cpuMhzFromStored(mhz) == 240 ? S::settings_cpu_240 : S::settings_cpu_160);
}

uint16_t otherCpuMhz(uint16_t mhz) { return cpuMhzFromStored(mhz) == 240 ? 160 : 240; }

CpuTap cpuTap(uint16_t saved, uint16_t running, bool pairing) {
  if (otherCpuMhz(saved) == running) return CpuTap::SaveOnly;  // (no restart: a pairing may go on)
  return pairing ? CpuTap::WaitPairing : CpuTap::AskRestart;
}

bool cpuRestartDue(uint32_t nowMs, uint32_t askedMs, bool linked, bool ampOn, bool libraryWrite) {
  if (libraryWrite) return false;  // the card's library is being written: never cut
  const int32_t elapsedMs = static_cast<int32_t>(nowMs - askedMs);  // < 0: asked later in this pass
  return (!linked && !ampOn) || elapsedMs >= static_cast<int32_t>(kRestartWaitMs);
}

const char* cpuSub(uint16_t saved, uint16_t running, char* buf, size_t size) {
  saved = cpuMhzFromStored(saved);
  if (running != saved && size > 0) {
    trf(buf, size, S::settings_cpu_pending_sub, running);
    return buf;
  }
  return tr(saved == 240 ? S::settings_cpu_240_sub : S::settings_cpu_160_sub);
}

void cpuDialogTitle(uint16_t mhz, char* buf, size_t size) {
  trf(buf, size, S::settings_cpu_dialog_title, cpuLabel(mhz));
}

const char* cpuDialogBody(uint16_t mhz) {
  return tr(cpuMhzFromStored(mhz) == 160 ? S::settings_cpu_dialog_body_160 : S::settings_cpu_dialog_body);
}

void cpuRestartingText(uint16_t mhz, char* buf, size_t size) {
  trf(buf, size, S::settings_cpu_restarting, cpuLabel(mhz));
}

void cpuBootText(uint16_t mhz, char* buf, size_t size) { trf(buf, size, S::settings_cpu_boot_note, cpuLabel(mhz)); }

// ---- Bluetooth power ----

TxLevels btLevels(int choice) { return kBtLevels[clampBt(choice)]; }

int btChoiceFromStored(int stored) { return clampBt(stored); }

const char* btLabel(int choice) { return tr(kBtLabel[clampBt(choice)]); }

int nextBt(int choice) { return (clampBt(choice) + 1) % kBtChoices; }

int levelDbm(int level) { return -12 + 3 * level; }

void btRangeText(int choice, char* buf, size_t size) {
  const TxLevels l = btLevels(choice);
  snprintf(buf, size, "%d..%+d dBm", levelDbm(l.min), levelDbm(l.max));
}

const char* btSub(int choice, bool pending) {
  return tr(pending ? S::settings_bt_pending_sub : kBtSub[clampBt(choice)]);
}

void BtLinkLevel::update(bool linked, int choice) {
  // A new link is made with the levels in force as it comes up.
  if (linked && !linked_) linkChoice_ = clampBt(choice);
  linked_ = linked;
}

// ---- About ----

void aboutText(uint16_t cpuMhz, int btChoice, char* buf, size_t size) {
  char range[16];
  btRangeText(btChoice, range, sizeof(range));
  trf(buf, size, S::about_power_value, cpuMhz, btLabel(btChoice), range);
}

}  // namespace powerchoice
