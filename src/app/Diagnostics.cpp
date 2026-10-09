// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/Diagnostics.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <atomic>

#include "app/Version.h"

namespace diag {

namespace {
// The low window (beginLowWindow()): the since-boot lowests when it began.
bool s_window = false;
uint32_t s_internalLowBefore = 0;
uint32_t s_psramLowBefore = 0;
std::atomic<uint32_t> s_failedAllocs{0};

// heap_caps' failed-allocation hook: on the task that asked, outside the
// heap's locks. No allocation and no lock of ours here: the ROM's printf.
void onAllocFailed(size_t size, uint32_t caps, const char* function) {
  const uint32_t n = s_failedAllocs.fetch_add(1) + 1;
  if (!(caps & MALLOC_CAP_SPIRAM) && size < 4096) return;
  if (n > 16 && n % 256 != 0) return;
  const char* task = pcTaskGetName(nullptr);
  esp_rom_printf("[heap] FAILED: %u B (caps 0x%x) in %s, task %s; PSRAM free %u B, largest block %u B (failed "
                 "allocations so far: %u)\n",
                 (unsigned)size, (unsigned)caps, function ? function : "?", task ? task : "?",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), (unsigned)n);
}
}  // namespace

uint32_t lowestSinceBoot(uint32_t caps) {
  const uint32_t now = static_cast<uint32_t>(heap_caps_get_minimum_free_size(caps));
  if (!s_window) return now;
  const uint32_t before = caps == MALLOC_CAP_SPIRAM ? s_psramLowBefore : s_internalLowBefore;
  return now < before ? now : before;
}

bool beginLowWindow() {
  if (s_window) return false;
  s_internalLowBefore = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  s_psramLowBefore = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
  s_window = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
  return s_window;
}

uint32_t endLowWindow() {
  const uint32_t low = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
  if (!s_window) return low;
  // (The IDF puts each heap's since-boot lowest back as the lower of the
  // two: nothing the window saw is lost.)
  heap_caps_monitor_local_minimum_free_size_stop();
  s_window = false;
  return low;
}

void watchFailedAllocs() { heap_caps_register_failed_alloc_callback(onAllocFailed); }
uint32_t failedAllocs() { return s_failedAllocs.load(); }

Heap heap() {
  return {
      static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
      lowestSinceBoot(MALLOC_CAP_INTERNAL),
      static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
      static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
      lowestSinceBoot(MALLOC_CAP_SPIRAM),
  };
}

void logHeap(const char* stage) {
  const Heap h = heap();
  Serial.printf("[heap] %-9s internal free=%luK min=%luK largest=%luK | psram free=%luK min=%luK\n", stage,
                (unsigned long)(h.internalFree / 1024), (unsigned long)(h.internalMin / 1024),
                (unsigned long)(h.internalLargest / 1024), (unsigned long)(h.psramFree / 1024),
                (unsigned long)(h.psramMin / 1024));
}

const char* boardName() {
  switch (M5.getBoard()) {
    case m5::board_t::board_M5StackCore2: return "M5Stack Core2";
    default: return "unknown";
  }
}

const char* pmicName() {
  switch (M5.Power.getType()) {
    case m5::Power_Class::pmic_axp192: return "AXP192";
    case m5::Power_Class::pmic_axp2101: return "AXP2101";
    case m5::Power_Class::pmic_unknown: return "none found";
    default: return "other";
  }
}

const char* imuName() {
  switch (M5.Imu.getType()) {
    case m5::imu_t::imu_bmi270: return "BMI270";
    case m5::imu_t::imu_mpu6886: return "MPU6886";
    case m5::imu_t::imu_none: return "none found";
    default: return "other";
  }
}

const char* resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default: return "other";
  }
}

static const char* typeName(esp_partition_type_t type) {
  switch (type) {
    case ESP_PARTITION_TYPE_APP: return "app";
    case ESP_PARTITION_TYPE_DATA: return "data";
    default: return "?";
  }
}

static const char* subtypeName(esp_partition_type_t type, esp_partition_subtype_t subtype) {
  if (type == ESP_PARTITION_TYPE_APP) {
    if (subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) return "factory";
    if (subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
      static const char* const kOta[] = {"ota_0", "ota_1", "ota_2", "ota_3", "ota_4", "ota_5", "ota_6", "ota_7",
                                         "ota_8", "ota_9", "ota_10", "ota_11", "ota_12", "ota_13", "ota_14", "ota_15"};
      return kOta[subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN];
    }
    return "?";
  }
  switch (subtype) {
    case ESP_PARTITION_SUBTYPE_DATA_OTA: return "ota";
    case ESP_PARTITION_SUBTYPE_DATA_PHY: return "phy";
    case ESP_PARTITION_SUBTYPE_DATA_NVS: return "nvs";
    case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "coredump";
    case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS: return "nvs_keys";
    case ESP_PARTITION_SUBTYPE_DATA_SPIFFS: return "spiffs";
    case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS: return "littlefs";
    case ESP_PARTITION_SUBTYPE_DATA_FAT: return "fat";
    default: return "?";
  }
}

// The OTA state otadata keeps for an app slot. A serial or single-file
// install writes boot_app0.bin there: one entry, sequence 1 (ota_0), state
// 0xFFFFFFFF (ESP_OTA_IMG_UNDEFINED), so ota_0 reads "undefined". "none":
// no valid entry for that slot.
static const char* otaStateName(const esp_partition_t* p) {
  esp_ota_img_states_t state;
  if (!p || esp_ota_get_state_partition(p, &state) != ESP_OK) return "none";
  switch (state) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
  }
}

void logRunningPartition() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (!running) {
    Serial.println("[flash] running partition unknown");
    return;
  }
  Serial.printf("[flash] running %s at 0x%lx (%luK, OTA state %s), next update %s", running->label,
                (unsigned long)running->address, (unsigned long)(running->size / 1024), otaStateName(running),
                next ? next->label : "none (no second OTA slot)");
  if (next) Serial.printf(" at 0x%lx", (unsigned long)next->address);
  Serial.println();
}

void printPartitionTable() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  Serial.printf("[flash] partition table as flashed (%lu MB chip):\n",
                (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)));
  Serial.println("[flash]   label     type subtype   offset    end       size");
  // In the table's order; esp_partition_next() releases the iterator after the last one.
  for (esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
       it; it = esp_partition_next(it)) {
    const esp_partition_t* p = esp_partition_get(it);
    Serial.printf("[flash] %c %-9s %-4s %-9s 0x%06lx  0x%06lx  %luK%s\n", p == running ? '*' : ' ', p->label,
                  typeName(p->type), subtypeName(p->type, p->subtype), (unsigned long)p->address,
                  (unsigned long)(p->address + p->size), (unsigned long)(p->size / 1024),
                  p->encrypted ? " (encrypted)" : "");
  }
  Serial.printf("[flash] * running %s (OTA state %s); boots %s; next update %s\n", running ? running->label : "?",
                otaStateName(running), boot ? boot->label : "?", next ? next->label : "none");
  // What an OTA update will compare: the running image's app description.
  Serial.printf("[flash] running app: version \"%s\" (app description), ELF %s\n", version::appDesc(),
                version::elfSha());
  nvs_stats_t nvs;
  if (nvs_get_stats(nullptr, &nvs) == ESP_OK) {
    Serial.printf("[flash] nvs: %u of %u entries used, %u namespaces\n", (unsigned)nvs.used_entries,
                  (unsigned)nvs.total_entries, (unsigned)nvs.namespace_count);
  }
}

}  // namespace diag
