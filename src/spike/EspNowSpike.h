// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once

// The ESP-NOW spike: what linking the WiFi driver and ESP-NOW costs, and what
// ESP-NOW carries between two Core2s (swap mode's go/no-go). Only a build with
// -DMSTREAM_ESPNOW_SPIKE=1 has it ([env:core2-espnow], never released), and
// only the console's E reaches it: no boot starts WiFi. It calls the IDF
// directly (esp_wifi_init, esp_wifi_set_mode(WIFI_MODE_STA), esp_wifi_start,
// esp_now_init): WiFi.h's WiFi.mode() would bring esp_netif and lwIP, which
// ESP-NOW doesn't need. WiFi keeps nothing in NVS.
//   E         status: up or not, this board's MAC, the peer and its rate, what arrived
//   Ei        up, on channel 1 (Ei on both boards)
//   Ep<mac>   the peer to send to (aa:bb:cc:dd:ee:ff: the other board's E), unencrypted
//   Er<rate>  the peer's PHY rate: 1 (ESP-NOW's own default, 11b), 11, 24, 54 (11g; the
//             default here) or m7 (HT20 MCS7, short GI)
//   Et<n>     n frames (1000 if none) of 1470 B, ESP-NOW v2's largest, to the peer, each
//             sent once the last one's send callback came (100 ms at most): the time,
//             the throughput, the callbacks' latency, the rate sent at and the failures
//             (the loop waits meanwhile). To the broadcast peer (Ep ff:ff:ff:ff:ff:ff)
//             it is a TX-only test: nothing acknowledges a broadcast
//   Ez        the receive counters zeroed
//   Ex        ESP-NOW and WiFi down again
//   Eb        restarts into a boot that never starts Bluetooth, the card or the UI
//             (swap mode's own boot): setup() stops after the banner and NVS, and only
//             E commands work; the restart after it boots normally (an RTC flag, not NVS)
// Ei, Et and Ex log the internal heap (free, largest block) and PSRAM at each
// step. While Bluetooth runs the IDF insists on WiFi's modem sleep (it aborts
// otherwise), so Ei leaves it on then and frames may be lost to it.
#ifndef MSTREAM_ESPNOW_SPIKE
#define MSTREAM_ESPNOW_SPIKE 0
#endif

#if MSTREAM_ESPNOW_SPIKE
namespace espnowspike {

// The console's E: the text after the letter (may be ""). Eb is main.cpp's:
// requestSwapBoot(), then its orderly restart.
void command(const char* arg);

// Eb: the next boot is swap mode's (setup() asks takeSwapBoot()).
void requestSwapBoot();
// True once after requestSwapBoot() and a restart (the flag cleared).
bool takeSwapBoot();
// That boot from setup(): E commands only, from Serial. Never returns.
[[noreturn]] void swapBoot();

}  // namespace espnowspike
#endif
