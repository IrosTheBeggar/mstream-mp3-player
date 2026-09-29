#pragma once
#include <cstddef>
#include <cstdint>

// The Output tab's "CPU speed" and "Bluetooth power" (docs/ENERGY.md items
// 6 and 7): their choices, labels and lines, the stored values' checks,
// and what a row says while a change waits for a restart or for the next
// connection. app/PowerSettings keeps them (NVS "power") and applies them.
//
// CPU speed: 240 or 160 MHz (kDefaultCpuMhz when nothing is saved). 240 <->
// 160 retunes the PLL the Bluetooth radio runs from, so it is set only at
// boot, before Bluetooth starts: a change is saved and takes a restart
// (asked first). The console's Pcb shares the stored value ("cpu_mhz": 160
// or 240; absent, or anything else, is the default).
//
// Bluetooth power: Low / Normal / High, the BR/EDR TX power levels the
// controller may use (esp_power_level_t: 0 = -12 dBm ... 7 = +9 dBm, 3 dB
// apart). Every range starts at the lowest level, so the headphones' power
// control can turn it down when they are close; the choice is the ceiling.
// Applied at once and at every stack start (before the first page), but a
// link that is up keeps its level (measured): a change while linked
// applies from the next connection, and the row says so until then
// (BtLinkLevel).
//
// Portable (host-tested: test_power_choices; the texts' widths in
// test_ui_library).
namespace powerchoice {

// ---- CPU speed ----

// 240: at 160 a list scrolled at half the frame rate while an MP3 played
// (ENERGY.md step 6a, measured); 160 stays a choice.
inline constexpr uint16_t kDefaultCpuMhz = 240;
// The row's two choices, in the pill's order.
inline constexpr int kCpuChoices = 2;
inline constexpr uint16_t kCpuMhz[kCpuChoices] = {240, 160};

// 160 or 240: the speeds the setting (and Pcb) may store.
bool validCpuMhz(uint32_t mhz);
// The speed a stored value asks for: a valid one as it is; absent (0) or
// anything else (80, a stray value) the default.
uint16_t cpuMhzFromStored(uint32_t stored);
// "240 MHz", "160 MHz" (the default's for anything else).
const char* cpuLabel(uint16_t mhz);
// The choice a tap asks for: the other one.
uint16_t otherCpuMhz(uint16_t mhz);

// A tap on the row, with `saved` the choice saved and `running` the clock
// set at boot: the other choice. Normally that needs a restart (asked first). When
// the saved choice already differs from the clock (the console's Pcb), the
// other one is what runs: it is saved and nothing restarts. While a pairing
// is under way (`pairing`) a restart would drop it, maybe half-bonded (the
// idle power-off waits for one too): it isn't offered, the toast says to
// wait (kCpuWaitPairing), and nothing is saved.
enum class CpuTap : uint8_t { AskRestart, SaveOnly, WaitPairing };
CpuTap cpuTap(uint16_t saved, uint16_t running, bool pairing);
// Once asked, the restart waits for the headphones to be let go and the
// speaker's amp to be switched off (both the orderly way: a clean
// disconnect, and the amp's enable dropped before its clock pins are
// reset, no pop), at most kRestartWaitMs (as the idle power-off waits).
inline constexpr uint32_t kRestartWaitMs = 3000;
// `askedMs` is when it was asked (millis()), `nowMs` the loop pass's time.
// A pass takes its time before the UI runs, so a restart asked during the
// pass is stamped after it: that counts as 0 ms waited, not a wrap to ~49
// days (which restarted at once, found on the device). millis() wrapping
// around is fine.
bool cpuRestartDue(uint32_t nowMs, uint32_t askedMs, bool linked, bool ampOn);
// The "Restarting at 160 MHz..." toast stays up this long: until the
// restart, which comes at most kRestartWaitMs after it.
inline constexpr uint32_t kRestartToastMs = kRestartWaitMs + 2000;
// The row's line: the choice's ("Smoothest lists and dancing"), or while
// the clock isn't the saved choice yet, what runs until a restart.
const char* cpuSub(uint16_t saved, uint16_t running, char* buf, size_t size);
// The dialog's title: "Restart at 160 MHz?"
void cpuDialogTitle(uint16_t mhz, char* buf, size_t size);
// The toast while it restarts: "Restarting at 160 MHz..."
void cpuRestartingText(uint16_t mhz, char* buf, size_t size);
// The next boot's toast: "CPU speed: 160 MHz".
void cpuBootText(uint16_t mhz, char* buf, size_t size);

// ---- Bluetooth power ----

enum BtChoice : uint8_t { kBtLow, kBtNormal, kBtHigh, kBtChoices };
inline constexpr int kDefaultBt = kBtNormal;

struct TxLevels {
  uint8_t min = 0, max = 0;  // esp_power_level_t, 0-7
};
TxLevels btLevels(int choice);
// A stored choice (NVS "bt_tx"; -1 absent): a valid one as it is, else Normal.
int btChoiceFromStored(int stored);
const char* btLabel(int choice);  // "Low", "Normal", "High"
// The next choice (a tap): after High, Low again.
int nextBt(int choice);
// A level's dBm: -12 + 3 x level.
int levelDbm(int level);
// "-12..+3 dBm"
void btRangeText(int choice, char* buf, size_t size);
// The row's line: the choice's ("Adjusts to the distance"), or "From the
// next connection" while the link that is up still has another choice's
// levels.
const char* btSub(int choice, bool pending);

// Which choice the link that is up was made with: a change while linked
// is pending until the next connection. Fed every loop pass.
class BtLinkLevel {
public:
  void update(bool linked, int choice);
  // Linked with another choice's levels than `choice`.
  bool pending(int choice) const { return linked_ && linkChoice_ != choice; }

private:
  bool linked_ = false;
  int linkChoice_ = kDefaultBt;
};

// ---- About ----

// "160 MHz, Bluetooth Normal (-12..+3 dBm)"
void aboutText(uint16_t cpuMhz, int btChoice, char* buf, size_t size);

}  // namespace powerchoice
