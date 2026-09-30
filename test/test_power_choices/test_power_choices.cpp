// Host tests for the CPU speed and Bluetooth power settings (PowerChoices,
// docs/ENERGY.md items 6 and 7): the stored values' checks and defaults,
// what a tap on the CPU row does, when its restart goes, the TX power
// ranges, the rows' lines while a change waits for a restart or the next
// connection, and About's text. The texts' widths are in test_ui_library.
// Run: pio test -e native
#include <unity.h>

#include <cstring>
#include <initializer_list>

#include "PowerChoices.h"
#include "UiText.h"

void setUp() {}
void tearDown() {}

namespace pc = powerchoice;

// NVS "power"/"cpu_mhz": 160 or 240 as they are; absent (0) and anything
// else (80, garbage, a Pcb of an older build) the default, 240 (ENERGY.md
// step 6a).
void test_cpu_stored_values() {
  TEST_ASSERT_EQUAL(240, pc::kDefaultCpuMhz);
  TEST_ASSERT_EQUAL(240, pc::cpuMhzFromStored(0));
  TEST_ASSERT_EQUAL(160, pc::cpuMhzFromStored(160));
  TEST_ASSERT_EQUAL(240, pc::cpuMhzFromStored(240));
  for (uint32_t bad : {1u, 80u, 120u, 159u, 161u, 239u, 241u, 480u, 65535u, 0xFFFFFFFFu}) {
    TEST_ASSERT_FALSE(pc::validCpuMhz(bad));
    TEST_ASSERT_EQUAL(pc::kDefaultCpuMhz, pc::cpuMhzFromStored(bad));
  }
  TEST_ASSERT_TRUE(pc::validCpuMhz(160));
  TEST_ASSERT_TRUE(pc::validCpuMhz(240));
  TEST_ASSERT_FALSE(pc::validCpuMhz(0));
  // Both choices are in the row, the default among them.
  TEST_ASSERT_EQUAL(2, pc::kCpuChoices);
  TEST_ASSERT_TRUE(pc::kCpuMhz[0] == pc::kDefaultCpuMhz || pc::kCpuMhz[1] == pc::kDefaultCpuMhz);
}

void test_cpu_labels_and_lines() {
  char buf[48];
  TEST_ASSERT_EQUAL_STRING("240 MHz", pc::cpuLabel(240));
  TEST_ASSERT_EQUAL_STRING("160 MHz", pc::cpuLabel(160));
  TEST_ASSERT_EQUAL_STRING("240 MHz", pc::cpuLabel(80));  // (the default's)
  TEST_ASSERT_EQUAL(160, pc::otherCpuMhz(240));
  TEST_ASSERT_EQUAL(240, pc::otherCpuMhz(160));
  TEST_ASSERT_EQUAL_STRING(uitext::kCpu240Sub, pc::cpuSub(240, 240, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING(uitext::kCpu160Sub, pc::cpuSub(160, 160, buf, sizeof(buf)));
  // Saved but not running yet (the console's Pcb since this boot): what
  // runs, until a restart.
  TEST_ASSERT_EQUAL_STRING("240 MHz until a restart", pc::cpuSub(160, 240, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("160 MHz until a restart", pc::cpuSub(240, 160, buf, sizeof(buf)));
  pc::cpuDialogTitle(160, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("Restart at 160 MHz?", buf);
  pc::cpuDialogTitle(240, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("Restart at 240 MHz?", buf);
  // To 160 the dialog says what it costs; to 240 only what the restart does.
  TEST_ASSERT_EQUAL_STRING(uitext::kCpuDialogBody160, pc::cpuDialogBody(160));
  TEST_ASSERT_EQUAL_STRING(uitext::kCpuDialogBody, pc::cpuDialogBody(240));
  TEST_ASSERT_EQUAL_STRING(uitext::kCpuDialogBody, pc::cpuDialogBody(80));  // (the default's)
  pc::cpuRestartingText(240, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("Restarting at 240 MHz\xE2\x80\xA6", buf);
  pc::cpuBootText(160, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("CPU speed: 160 MHz", buf);
  pc::cpuBootText(240, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("CPU speed: 240 MHz", buf);
}

// A tap asks for the other choice. Normally that takes a restart (asked
// first); when the saved choice already differs from the clock (Pcb), the
// other choice is what runs: saved, no restart.
void test_cpu_tap() {
  TEST_ASSERT_EQUAL(pc::CpuTap::AskRestart, pc::cpuTap(160, 160, false));
  TEST_ASSERT_EQUAL(pc::CpuTap::AskRestart, pc::cpuTap(240, 240, false));
  TEST_ASSERT_EQUAL(pc::CpuTap::SaveOnly, pc::cpuTap(160, 240, false));
  TEST_ASSERT_EQUAL(pc::CpuTap::SaveOnly, pc::cpuTap(240, 160, false));
  // A boot clock the setting doesn't offer (a failed switch): a change
  // still restarts.
  TEST_ASSERT_EQUAL(pc::CpuTap::AskRestart, pc::cpuTap(160, 80, false));
  // A pairing under way: no restart offered (it would drop the pairing);
  // a change that needs none is still saved.
  TEST_ASSERT_EQUAL(pc::CpuTap::WaitPairing, pc::cpuTap(160, 160, true));
  TEST_ASSERT_EQUAL(pc::CpuTap::WaitPairing, pc::cpuTap(240, 240, true));
  TEST_ASSERT_EQUAL(pc::CpuTap::WaitPairing, pc::cpuTap(160, 80, true));
  TEST_ASSERT_EQUAL(pc::CpuTap::SaveOnly, pc::cpuTap(160, 240, true));
}

// Once asked, the restart waits for the headphones to go and the speaker's
// amp to be off (a clean disconnect; no pop through the reset), at most 3 s.
void test_cpu_restart_waits_for_the_headphones_and_the_amp() {
  TEST_ASSERT_EQUAL(3000u, pc::kRestartWaitMs);
  const uint32_t at = 50000;  // asked at
  // Nothing to wait for: at once.
  TEST_ASSERT_TRUE(pc::cpuRestartDue(at, at, false, false));
  // The headphones still linked, or the amp still on (paused on the
  // speaker: its fade still playing out): it waits.
  TEST_ASSERT_FALSE(pc::cpuRestartDue(at, at, true, false));
  TEST_ASSERT_FALSE(pc::cpuRestartDue(at, at, false, true));
  TEST_ASSERT_FALSE(pc::cpuRestartDue(at + 2999, at, true, true));
  TEST_ASSERT_FALSE(pc::cpuRestartDue(at + 2999, at, false, true));
  // Both done: at once, whenever.
  TEST_ASSERT_TRUE(pc::cpuRestartDue(at + 40, at, false, false));
  // At most 3 s, whatever is left.
  TEST_ASSERT_TRUE(pc::cpuRestartDue(at + 3000, at, true, false));
  TEST_ASSERT_TRUE(pc::cpuRestartDue(at + 3000, at, false, true));
  TEST_ASSERT_TRUE(pc::cpuRestartDue(at + 3000, at, true, true));
  // Asked during the loop pass that runs the check: the pass's time is
  // from before the tap. That is no time waited, not a wrap to ~49 days
  // (on the device it restarted at once, the headphones still linked and
  // the amp still on).
  TEST_ASSERT_FALSE(pc::cpuRestartDue(at - 7, at, true, false));
  TEST_ASSERT_FALSE(pc::cpuRestartDue(at - 7, at, false, true));
  TEST_ASSERT_TRUE(pc::cpuRestartDue(at - 7, at, false, false));
  // millis() wrapping around between the ask and now.
  TEST_ASSERT_FALSE(pc::cpuRestartDue(0x00000100u, 0xFFFFFF00u, true, true));  // 512 ms
  TEST_ASSERT_TRUE(pc::cpuRestartDue(0x00000C00u, 0xFFFFFF00u, true, true));   // 3328 ms
  // The toast stays up past the longest wait (the screen never goes dark
  // without it).
  TEST_ASSERT_TRUE(pc::kRestartToastMs > pc::kRestartWaitMs);
}

// esp_power_level_t 0-7 is -12..+9 dBm in 3 dB steps. Every range starts at
// the lowest level (the headphones' power control can turn it down); the
// choice is the ceiling. Normal keeps today's ceiling (+3 dBm).
void test_bt_levels() {
  TEST_ASSERT_EQUAL(-12, pc::levelDbm(0));
  TEST_ASSERT_EQUAL(3, pc::levelDbm(5));
  TEST_ASSERT_EQUAL(9, pc::levelDbm(7));
  TEST_ASSERT_EQUAL(3, pc::kBtChoices);
  TEST_ASSERT_EQUAL(pc::kBtNormal, pc::kDefaultBt);
  const pc::TxLevels low = pc::btLevels(pc::kBtLow), normal = pc::btLevels(pc::kBtNormal),
                     high = pc::btLevels(pc::kBtHigh);
  TEST_ASSERT_EQUAL(0, low.min);
  TEST_ASSERT_EQUAL(2, low.max);
  TEST_ASSERT_EQUAL(0, normal.min);
  TEST_ASSERT_EQUAL(5, normal.max);
  TEST_ASSERT_EQUAL(0, high.min);
  TEST_ASSERT_EQUAL(7, high.max);
  for (int c = 0; c < pc::kBtChoices; ++c) {
    const pc::TxLevels l = pc::btLevels(c);
    TEST_ASSERT_TRUE(l.min <= l.max);
    TEST_ASSERT_TRUE(l.max <= 7);
  }
  char buf[24];
  pc::btRangeText(pc::kBtLow, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("-12..-6 dBm", buf);
  pc::btRangeText(pc::kBtNormal, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("-12..+3 dBm", buf);
  pc::btRangeText(pc::kBtHigh, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("-12..+9 dBm", buf);
}

// NVS "power"/"bt_tx": the choice's index; absent (-1) or out of range is
// Normal. A tap takes the next (after High, Low).
void test_bt_stored_values_and_taps() {
  TEST_ASSERT_EQUAL(pc::kBtNormal, pc::btChoiceFromStored(-1));
  TEST_ASSERT_EQUAL(pc::kBtLow, pc::btChoiceFromStored(0));
  TEST_ASSERT_EQUAL(pc::kBtNormal, pc::btChoiceFromStored(1));
  TEST_ASSERT_EQUAL(pc::kBtHigh, pc::btChoiceFromStored(2));
  TEST_ASSERT_EQUAL(pc::kBtNormal, pc::btChoiceFromStored(3));
  TEST_ASSERT_EQUAL(pc::kBtNormal, pc::btChoiceFromStored(255));
  TEST_ASSERT_EQUAL(pc::kBtHigh, pc::nextBt(pc::kBtNormal));
  TEST_ASSERT_EQUAL(pc::kBtLow, pc::nextBt(pc::kBtHigh));
  TEST_ASSERT_EQUAL(pc::kBtNormal, pc::nextBt(pc::kBtLow));
  TEST_ASSERT_EQUAL_STRING("Low", pc::btLabel(pc::kBtLow));
  TEST_ASSERT_EQUAL_STRING("Normal", pc::btLabel(pc::kBtNormal));
  TEST_ASSERT_EQUAL_STRING("High", pc::btLabel(pc::kBtHigh));
  TEST_ASSERT_EQUAL_STRING(uitext::kBtLowSub, pc::btSub(pc::kBtLow, false));
  TEST_ASSERT_EQUAL_STRING(uitext::kBtNormalSub, pc::btSub(pc::kBtNormal, false));
  TEST_ASSERT_EQUAL_STRING(uitext::kBtHighSub, pc::btSub(pc::kBtHigh, false));
  TEST_ASSERT_EQUAL_STRING(uitext::kBtPendingSub, pc::btSub(pc::kBtHigh, true));
}

// A link that is up keeps its level: a change while linked is pending until
// the next connection, and only while the link lasts.
void test_bt_pending_until_the_next_connection() {
  pc::BtLinkLevel l;
  int choice = pc::kBtNormal;
  // Unlinked: a change applies to the next page; nothing pending.
  l.update(false, choice);
  choice = pc::kBtLow;
  l.update(false, choice);
  TEST_ASSERT_FALSE(l.pending(choice));
  // Linked with Low.
  l.update(true, choice);
  TEST_ASSERT_FALSE(l.pending(choice));
  // Changed while linked: pending.
  choice = pc::kBtHigh;
  l.update(true, choice);
  TEST_ASSERT_TRUE(l.pending(choice));
  // Back to the link's own choice: nothing pending.
  choice = pc::kBtLow;
  l.update(true, choice);
  TEST_ASSERT_FALSE(l.pending(choice));
  choice = pc::kBtNormal;
  l.update(true, choice);
  TEST_ASSERT_TRUE(l.pending(choice));
  // The link goes: nothing pending (the next one gets the new levels).
  l.update(false, choice);
  TEST_ASSERT_FALSE(l.pending(choice));
  // The next connection has them.
  l.update(true, choice);
  TEST_ASSERT_FALSE(l.pending(choice));
  // Linked at boot (the first pass sees the link up): made with the choice then.
  pc::BtLinkLevel boot;
  boot.update(true, pc::kBtHigh);
  TEST_ASSERT_FALSE(boot.pending(pc::kBtHigh));
  TEST_ASSERT_TRUE(boot.pending(pc::kBtLow));
}

void test_about_text() {
  char buf[64];
  pc::aboutText(160, pc::kBtNormal, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("160 MHz; Normal (-12..+3 dBm)", buf);
  pc::aboutText(240, pc::kBtHigh, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("240 MHz; High (-12..+9 dBm)", buf);
  pc::aboutText(160, pc::kBtLow, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("160 MHz; Low (-12..-6 dBm)", buf);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_cpu_stored_values);
  RUN_TEST(test_cpu_labels_and_lines);
  RUN_TEST(test_cpu_tap);
  RUN_TEST(test_cpu_restart_waits_for_the_headphones_and_the_amp);
  RUN_TEST(test_bt_levels);
  RUN_TEST(test_bt_stored_values_and_taps);
  RUN_TEST(test_bt_pending_until_the_next_connection);
  RUN_TEST(test_about_text);
  return UNITY_END();
}
