#include "app/Diagnostics.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

namespace diag {

Heap heap() {
  return {
      static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
      static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
      static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
      static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
  };
}

void logHeap(const char* stage) {
  const Heap h = heap();
  Serial.printf("[heap] %-9s internal free=%luK min=%luK largest=%luK | psram free=%luK\n", stage,
                (unsigned long)(h.internalFree / 1024), (unsigned long)(h.internalMin / 1024),
                (unsigned long)(h.internalLargest / 1024), (unsigned long)(h.psramFree / 1024));
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

}  // namespace diag
