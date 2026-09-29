#include "PowerChoices.h"

#include <cstdio>

#include "UiText.h"

namespace powerchoice {

namespace {
constexpr TxLevels kBtLevels[kBtChoices] = {{0, 2}, {0, 5}, {0, 7}};  // -12..-6, -12..+3, -12..+9 dBm
constexpr const char* kBtLabel[kBtChoices] = {"Low", "Normal", "High"};
constexpr const char* kBtSub[kBtChoices] = {uitext::kBtLowSub, uitext::kBtNormalSub, uitext::kBtHighSub};

int clampBt(int c) { return c < 0 || c >= kBtChoices ? kDefaultBt : c; }
}  // namespace

// ---- CPU speed ----

bool validCpuMhz(uint32_t mhz) { return mhz == 160 || mhz == 240; }

uint16_t cpuMhzFromStored(uint32_t stored) {
  return validCpuMhz(stored) ? static_cast<uint16_t>(stored) : kDefaultCpuMhz;
}

const char* cpuLabel(uint16_t mhz) { return cpuMhzFromStored(mhz) == 240 ? "240 MHz" : "160 MHz"; }

uint16_t otherCpuMhz(uint16_t mhz) { return cpuMhzFromStored(mhz) == 240 ? 160 : 240; }

CpuTap cpuTap(uint16_t saved, uint16_t running, bool pairing) {
  if (otherCpuMhz(saved) == running) return CpuTap::SaveOnly;  // (no restart: a pairing may go on)
  return pairing ? CpuTap::WaitPairing : CpuTap::AskRestart;
}

bool cpuRestartDue(uint32_t nowMs, uint32_t askedMs, bool linked, bool ampOn) {
  const int32_t elapsedMs = static_cast<int32_t>(nowMs - askedMs);  // < 0: asked later in this pass
  return (!linked && !ampOn) || elapsedMs >= static_cast<int32_t>(kRestartWaitMs);
}

const char* cpuSub(uint16_t saved, uint16_t running, char* buf, size_t size) {
  saved = cpuMhzFromStored(saved);
  if (running != saved && size > 0) {
    snprintf(buf, size, uitext::kCpuPendingSub, static_cast<unsigned>(running));
    return buf;
  }
  return saved == 240 ? uitext::kCpu240Sub : uitext::kCpu160Sub;
}

void cpuDialogTitle(uint16_t mhz, char* buf, size_t size) { snprintf(buf, size, "Restart at %s?", cpuLabel(mhz)); }

void cpuRestartingText(uint16_t mhz, char* buf, size_t size) {
  snprintf(buf, size, "Restarting at %s\xE2\x80\xA6", cpuLabel(mhz));
}

void cpuBootText(uint16_t mhz, char* buf, size_t size) { snprintf(buf, size, "CPU speed: %s", cpuLabel(mhz)); }

// ---- Bluetooth power ----

TxLevels btLevels(int choice) { return kBtLevels[clampBt(choice)]; }

int btChoiceFromStored(int stored) { return clampBt(stored); }

const char* btLabel(int choice) { return kBtLabel[clampBt(choice)]; }

int nextBt(int choice) { return (clampBt(choice) + 1) % kBtChoices; }

int levelDbm(int level) { return -12 + 3 * level; }

void btRangeText(int choice, char* buf, size_t size) {
  const TxLevels l = btLevels(choice);
  snprintf(buf, size, "%d..%+d dBm", levelDbm(l.min), levelDbm(l.max));
}

const char* btSub(int choice, bool pending) { return pending ? uitext::kBtPendingSub : kBtSub[clampBt(choice)]; }

void BtLinkLevel::update(bool linked, int choice) {
  // A new link is made with the levels in force as it comes up.
  if (linked && !linked_) linkChoice_ = clampBt(choice);
  linked_ = linked;
}

// ---- About ----

void aboutText(uint16_t cpuMhz, int btChoice, char* buf, size_t size) {
  char range[16];
  btRangeText(btChoice, range, sizeof(range));
  snprintf(buf, size, "%u MHz; %s (%s)", static_cast<unsigned>(cpuMhz), btLabel(btChoice), range);
}

}  // namespace powerchoice
