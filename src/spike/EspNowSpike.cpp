// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "spike/EspNowSpike.h"

#if MSTREAM_ESPNOW_SPIKE

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_bt.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace espnowspike {
namespace {

constexpr uint8_t kChannel = 1;
constexpr size_t kFrame = ESP_NOW_MAX_DATA_LEN_V2;  // 1470 B
constexpr uint32_t kSendTimeoutMs = 100;
constexpr long kDefaultFrames = 1000;
constexpr uint32_t kSwapBootMagic = 0x53574150;  // "SWAP"

struct Rate {
  const char* name;
  wifi_phy_mode_t mode;
  wifi_phy_rate_t rate;
};
constexpr Rate kRates[] = {
    {"1", WIFI_PHY_MODE_11B, WIFI_PHY_RATE_1M_L},  // ESP-NOW's own default
    {"11", WIFI_PHY_MODE_11B, WIFI_PHY_RATE_11M_S},
    {"24", WIFI_PHY_MODE_11G, WIFI_PHY_RATE_24M},
    {"54", WIFI_PHY_MODE_11G, WIFI_PHY_RATE_54M},
    {"m7", WIFI_PHY_MODE_HT20, WIFI_PHY_RATE_MCS7_SGI},
};
constexpr size_t kDefaultRate = 3;  // 54 Mbps

bool up = false;
bool hasPeer = false;
uint8_t peer[ESP_NOW_ETH_ALEN] = {};
size_t rate = kDefaultRate;
SemaphoreHandle_t sendDone = nullptr;  // given by onSent()
std::atomic<bool> sendOk{false};
std::atomic<int> sentRate{-1};         // the rate the driver says the last frame went at

// What arrived, from anyone (the WiFi task counts, the console reads).
std::atomic<uint32_t> rxFrames{0};
std::atomic<uint32_t> rxBytes{0};
std::atomic<uint32_t> rxFirstUs{0};
std::atomic<uint32_t> rxLastUs{0};
std::atomic<int> rxRssi{0};

// Survives esp_restart() (not a power cut): Eb's boot, without NVS.
RTC_NOINIT_ATTR uint32_t swapBootFlag;

void onSent(const esp_now_send_info_t* info, esp_now_send_status_t status) {
  sendOk = status == ESP_NOW_SEND_SUCCESS;
  if (info) sentRate = info->rate;
  xSemaphoreGive(sendDone);
}

void onReceived(const esp_now_recv_info_t* info, const uint8_t*, int len) {
  const uint32_t now = micros();
  if (rxFrames.load() == 0) rxFirstUs = now;
  rxLastUs = now;
  rxBytes += static_cast<uint32_t>(len);
  rxFrames++;
  if (info->rx_ctrl) rxRssi = info->rx_ctrl->rssi;
}

bool ok(esp_err_t err, const char* what) {
  if (err == ESP_OK) return true;
  Serial.printf("[espnow] %s: %s\n", what, esp_err_to_name(err));
  return false;
}

unsigned long internalFree() { return static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); }

// The internal heap as the [heap] lines read it (MALLOC_CAP_INTERNAL), and PSRAM.
void heapLine(const char* stage) {
  Serial.printf("[espnow] heap %-22s internal %6lu B free, largest block %6lu B; PSRAM %7lu B free\n", stage,
                internalFree(), static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

void status() {
  uint8_t mac[ESP_NOW_ETH_ALEN] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.printf("[espnow] %s; this board %02x:%02x:%02x:%02x:%02x:%02x, channel %u", up ? "up" : "down (Ei)", mac[0],
                mac[1], mac[2], mac[3], mac[4], mac[5], kChannel);
  if (hasPeer) {
    Serial.printf("; peer %02x:%02x:%02x:%02x:%02x:%02x at %s", peer[0], peer[1], peer[2], peer[3], peer[4], peer[5],
                  kRates[rate].name);
  }
  const uint32_t frames = rxFrames, bytes = rxBytes, us = rxLastUs - rxFirstUs;
  Serial.printf("; received %lu frames, %lu B", static_cast<unsigned long>(frames), static_cast<unsigned long>(bytes));
  if (frames > 1 && us > 0) {
    Serial.printf(" over %lu ms (%.0f kbit/s), the last at %d dBm", static_cast<unsigned long>(us / 1000),
                  bytes * 8000.0 / us, rxRssi.load());
  }
  Serial.println();
}

void bringUp() {
  if (up) {
    Serial.println("[espnow] already up");
    return;
  }
  const bool bt = esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED;
  Serial.printf("[espnow] Bluetooth controller %s\n", bt ? "enabled" : "not enabled");
  heapLine("before:");
  const uint32_t t0 = millis();
  // The driver posts its events to the default loop (nothing here listens).
  const esp_err_t loop = esp_event_loop_create_default();
  if (loop != ESP_ERR_INVALID_STATE && !ok(loop, "esp_event_loop_create_default")) return;
  heapLine("after the event loop:");
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  cfg.nvs_enable = 0;  // nothing of WiFi's in NVS
  if (!ok(esp_wifi_init(&cfg), "esp_wifi_init")) return;
  heapLine("after esp_wifi_init:");
  if (!ok(esp_wifi_set_mode(WIFI_MODE_STA), "esp_wifi_set_mode") || !ok(esp_wifi_start(), "esp_wifi_start")) {
    esp_wifi_deinit();
    return;
  }
  heapLine("after esp_wifi_start:");
  if ((!bt && !ok(esp_wifi_set_ps(WIFI_PS_NONE), "esp_wifi_set_ps")) ||
      !ok(esp_wifi_set_channel(kChannel, WIFI_SECOND_CHAN_NONE), "esp_wifi_set_channel") ||
      !ok(esp_now_init(), "esp_now_init")) {
    esp_wifi_stop();
    esp_wifi_deinit();
    return;
  }
  heapLine("after esp_now_init:");
  if (!sendDone) sendDone = xSemaphoreCreateBinary();
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onReceived);
  uint32_t version = 0;
  esp_now_get_version(&version);
  up = true;
  Serial.printf("[espnow] up in %lu ms: ESP-NOW v%lu on channel %u, %s\n", static_cast<unsigned long>(millis() - t0),
                static_cast<unsigned long>(version), kChannel,
                bt ? "modem sleep on (Bluetooth runs)" : "no power save");
}

bool applyRate() {
  esp_now_rate_config_t cfg = {};
  cfg.phymode = kRates[rate].mode;
  cfg.rate = kRates[rate].rate;
  return ok(esp_now_set_peer_rate_config(peer, &cfg), "esp_now_set_peer_rate_config");
}

void setPeer(const char* text) {
  unsigned v[ESP_NOW_ETH_ALEN];
  if (!up || sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
    Serial.println(up ? "[espnow] Ep<aa:bb:cc:dd:ee:ff>: the other board's MAC (its E)" : "[espnow] down: Ei first");
    return;
  }
  if (hasPeer) esp_now_del_peer(peer);
  hasPeer = false;
  esp_now_peer_info_t info = {};
  for (size_t i = 0; i < ESP_NOW_ETH_ALEN; ++i) info.peer_addr[i] = static_cast<uint8_t>(v[i]);
  info.channel = kChannel;
  info.ifidx = WIFI_IF_STA;
  info.encrypt = false;
  if (!ok(esp_now_add_peer(&info), "esp_now_add_peer")) return;
  memcpy(peer, info.peer_addr, sizeof(peer));
  hasPeer = true;
  if (applyRate()) status();
}

void setRate(const char* text) {
  for (size_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); ++i) {
    if (strcmp(text, kRates[i].name) != 0) continue;
    rate = i;
    if (!hasPeer || applyRate()) Serial.printf("[espnow] rate %s%s\n", kRates[i].name, hasPeer ? "" : " (for Ep)");
    return;
  }
  Serial.println("[espnow] Er<rate>: 1, 11, 24, 54 or m7");
}

void sendTest(long n) {
  if (!up || !hasPeer) {
    Serial.println(up ? "[espnow] no peer: Ep<mac> first" : "[espnow] down: Ei first");
    return;
  }
  if (n <= 0) n = kDefaultFrames;
  uint8_t* frame = static_cast<uint8_t*>(heap_caps_malloc(kFrame, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!frame) {
    Serial.println("[espnow] no internal RAM for the frame");
    return;
  }
  for (size_t i = 0; i < kFrame; ++i) frame[i] = static_cast<uint8_t>(i);
  heapLine("before Et:");
  unsigned long acked = 0, failed = 0, late = 0, refused = 0;
  uint32_t cbMin = UINT32_MAX, cbMax = 0;
  uint64_t cbSum = 0;
  sentRate = -1;
  const uint32_t t0 = micros();
  for (long i = 0; i < n; ++i) {
    memcpy(frame, &i, sizeof(i));     // a sequence number
    xSemaphoreTake(sendDone, 0);      // a callback that came after its timeout
    const uint32_t sentAt = micros();
    const esp_err_t err = esp_now_send(peer, frame, kFrame);
    if (err != ESP_OK) {
      if (refused++ == 0) Serial.printf("[espnow] esp_now_send: %s\n", esp_err_to_name(err));
      vTaskDelay(1);
      continue;
    }
    if (xSemaphoreTake(sendDone, pdMS_TO_TICKS(kSendTimeoutMs)) != pdTRUE) {
      late++;
      continue;
    }
    const uint32_t cb = micros() - sentAt;
    cbMin = cb < cbMin ? cb : cbMin;
    cbMax = cb > cbMax ? cb : cbMax;
    cbSum += cb;
    if (sendOk) {
      acked++;
    } else {
      failed++;
    }
  }
  const uint32_t us = micros() - t0;
  heapLine("after Et:");
  heap_caps_free(frame);
  const unsigned long called = acked + failed;
  Serial.printf("[espnow] %ld frames of %u B at %s in %lu ms: %lu sent ok (%.0f kbit/s of payload), %lu failed, "
                "%lu timed out, %lu refused\n",
                n, static_cast<unsigned>(kFrame), kRates[rate].name, static_cast<unsigned long>(us / 1000), acked,
                us ? acked * kFrame * 8000.0 / us : 0.0, failed, late, refused);
  if (called) {
    Serial.printf("[espnow] send to callback: %lu us on average, %lu-%lu us; the driver's rate code for the last: "
                  "0x%02x\n",
                  static_cast<unsigned long>(cbSum / called), static_cast<unsigned long>(cbMin),
                  static_cast<unsigned long>(cbMax), static_cast<unsigned>(sentRate.load()));
  }
}

void bringDown() {
  if (!up) {
    Serial.println("[espnow] already down");
    return;
  }
  esp_now_deinit();  // (drops the peer)
  esp_wifi_stop();
  esp_wifi_deinit();
  up = hasPeer = false;
  heapLine("after Ex:");
}

}  // namespace

void command(const char* arg) {
  switch (arg[0]) {
    case '\0': status(); break;
    case 'i': bringUp(); break;
    case 'p': setPeer(arg + 1); break;
    case 'r': setRate(arg + 1); break;
    case 't': sendTest(atol(arg + 1)); break;
    case 'z':
      rxFrames = 0;
      rxBytes = 0;
      Serial.println("[espnow] receive counters zeroed");
      break;
    case 'x': bringDown(); break;
    default:
      Serial.println("[espnow] E status, Ei up, Ep<mac> the peer, Er<1|11|24|54|m7> its rate, Et<n> n frames to it, "
                     "Ez the counters zeroed, Ex down, Eb a boot without Bluetooth");
      break;
  }
}

void requestSwapBoot() { swapBootFlag = kSwapBootMagic; }

bool takeSwapBoot() {
  const bool asked = swapBootFlag == kSwapBootMagic;
  swapBootFlag = 0;
  return asked;
}

void swapBoot() {
  Serial.println("[espnow] Eb's boot: no Bluetooth, card or UI; E commands only (Enter after each), and the next "
                 "restart boots normally");
  heapLine("at the boot:");
  char line[64];
  size_t n = 0;
  for (;;) {
    while (Serial.available() > 0) {
      const char c = static_cast<char>(Serial.read());
      if (c != '\r' && c != '\n') {
        if (n + 1 < sizeof(line)) line[n++] = c;
        continue;
      }
      line[n] = '\0';
      if (n > 0 && line[0] == 'E') {
        Serial.printf("> esp-now \"%s\"\n", line + 1);
        command(line + 1);
      } else if (n > 0) {
        Serial.printf("[espnow] \"%s\": E commands only in this boot\n", line);
      }
      n = 0;
    }
    delay(5);
  }
}

}  // namespace espnowspike

#endif  // MSTREAM_ESPNOW_SPIKE
