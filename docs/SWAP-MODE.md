# Swap mode: research, a design and the ESP-NOW link spike

Swap mode is the user's idea (2026-10-19). Players in swap mode find
each other nearby. One asks another to connect, and the listener of the
asked player approves on its screen. The two players are then linked,
and either can send the other files. This document is the research
behind it:

- what the radio and the firmware allow;
- the radio options;
- a proposed design;
- a spike that linked the WiFi driver and ESP-NOW into the firmware and
  measured both, on the build and on a Core2.

In short: it is feasible. The WiFi driver and ESP-NOW fit in IRAM: they
take 2,676 B of the 5,161 B left. A boot that never starts Bluetooth
keeps 130 KB of internal RAM with ESP-NOW up. One Core2 sent 1,470-byte
frames at 22.6 Mbit/s.

So the design:

- swap mode is its own boot, without Bluetooth;
- ESP-NOW carries everything: the beacons, the request, the approval
  and the files;
- the files land in `/music/Received/`, and the next normal boot's walk
  and tag scan add them to the library.

Two things aren't known yet: a transfer between two Core2s (only one was
at hand) and what swap mode costs the battery.

**Status (2026-10-20):** research, a spike and a card bench. Nothing of
swap mode is built, and the spike's code is not in the tree (section
5.1). The card's write bench is (`gx`, `gxs`, `gxc`: section 7.2), and
so is the driver's faster CRC16 (7.2.1).

The other documents:

- **docs/ARCHITECTURE.md:** "Bluetooth", "Storage", "Build notes"
  (IRAM) and the roadmap. Its WiFi sync is "its own mode" for the reason
  swap mode is.
- **docs/POC-RESULTS.md:** internal RAM with Bluetooth, and the first
  IRAM budget.
- **docs/METADATA.md:** the card contract: 1.2 (cards filled by hand),
  2.8 (paths), 2.11 (who may write what), 3.2.3 (the boot's walk).
- **docs/ENERGY.md:** the battery, and the `custom_sdkconfig` rebuild
  (P3b).
- **docs/USB-VISUALIZER.md:** "Sharing the framing with pairing and
  Wi-Fi": the `@` lines a computer could name a player with.

Labels, as elsewhere in the repo, plus two for other people's numbers:

- **READ:** from the code, or from the framework's own files.
- **MEASURED:** from a build or on the Core2, with the date.
- **ESTIMATED:** worked out from measured or documented figures; not
  measured.
- **DOCUMENTED:** Espressif's documentation or datasheet (section 11).
- **REPORTED:** someone else's measurement, on other boards (section 11).

"The listener" is the person holding a player; "the user" is the
project's owner, who decides.

---

## 0. In one screen

- **What:** a mode the listener enters on purpose.
  - Nearby players in it list each other.
  - A asks B, and B's listener checks a code and accepts.
  - While linked, either side sends files.
- **Feasible:** yes. The link fits, and the radio is fast (section 5).
- **Its own boot.**
  - Entering restarts the player into a boot that never starts
    Bluetooth. Leaving restarts it again.
  - Why: the radio is shared, and Bluetooth's stack takes about 70 KB of
    internal RAM.
  - MEASURED: ESP-NOW beside Bluetooth left 21 KB free (12 KB at its
    low point); without Bluetooth, 130 KB.
- **ESP-NOW for everything:** Espressif's connectionless frames on the
  WiFi radio, with no router, no password and no IP stack. A v2 frame
  carries 1,470 bytes.
- **Approval:**
  - The players exchange X25519 keys, then show the same 6-digit code
    on both screens.
  - B's Accept links them.
  - The shared key becomes the ESP-NOW link's encryption key.
- **Files:**
  - offered with their name, size and CRC-32;
  - sent as acknowledged frames;
  - written to a hidden `.part` file in 32 KB pieces, its room reserved
    first;
  - checked, then renamed into `/music/Received/<sender>/`.
- **The library:** nothing new. Leaving swap mode restarts the player,
  and the walk and the tag scan take the new files as they take files a
  listener copied on.
- **What it costs** (MEASURED, a build of 2026-10-19):
  - IRAM: +2,676 B, leaving 2,485 B;
  - static internal RAM: +15,812 B, in every boot;
  - flash: +374 KB, 75 % of the slot.
- **Speed:**
  - MEASURED: the radio sent 22.6 Mbit/s at 54 Mbps. The card takes
    1.53 MB/s in 32 KB pieces (1.02 in today's 4 KB pieces) and gives
    1.36-1.39 MB/s back, with the driver's CRC16 two bytes a step since
    2026-10-20 (1.47 and 1.31-1.34 before). 20 MHz is its clock's
    ceiling.
  - ESTIMATED: the sender's card reads set the pace, at about 1.35 MB/s
    end to end, so a 5 MB MP3 takes about 4 s.
- **Open:**
  - a transfer between two Core2s;
  - the battery;
  - normal mode's internal RAM (section 8);
  - the user's decisions (section 10).

---

## 1. What swap mode is

### 1.1 The user's description (2026-10-19)

1. Only players in swap mode can be found (outside it the radio is off).
2. A link joins two players. One asks for it, and the other's listener
   approves it there.
3. While linked, either side can send files to the other.

### 1.2 What this design adds, and leaves out

- **It adds:**
  - a code on both screens, so B's listener knows the request is A's;
  - encryption;
  - a type, path and room check on what arrives;
  - an exit by itself.
- **It leaves out:**
  - more than one link at a time;
  - anything over the internet or through mStream;
  - listening on headphones while in swap mode (section 2.1);
  - sending anything but what the library lists, plus its albums'
    covers.

---

## 2. What the radio and the firmware allow

### 2.1 One radio

The ESP32 has one 2.4 GHz radio for WiFi and Bluetooth, shared in time
by its coexistence scheme. Espressif's table against Classic Bluetooth
(DOCUMENTED, IDF 5.5):

| WiFi activity | Against Classic Bluetooth |
|---|---|
| ESP-NOW TX | stable |
| ESP-NOW RX | stable in STA mode only |
| SoftAP connecting or connected | "supported but the performance is unstable" |

The table says nothing about A2DP.

The spike measured what it means here (MEASURED 2026-10-19, section
5.3). Beside Bluetooth, ESP-NOW took internal RAM down to 21 KB, with a
low point of 12 KB. The IDF forced WiFi's modem sleep, and HT20 MCS7
frames stalled for up to 61 ms. The roadmap already makes WiFi sync its
own mode for the same reason (ARCHITECTURE.md, Roadmap item 1).

### 2.2 The firmware today

- **No WiFi** (READ): nothing links the driver. The spike's baseline
  image has no `esp_wifi_init`, `ieee80211_*` or `pp*` symbols.
- **IRAM:** 125,911 of 131,072 B used, 5,161 B free (MEASURED
  2026-10-19, main at b78002e).
- **Internal RAM:**
  - Bluetooth's stack claims about 70 KB at boot (ARCHITECTURE.md,
    "Tasks and cores").
  - The 0.11.0 checks' lowest, while a card step ran, was 53,012 B
    (RELEASE-0.11.0-CHECKS.md).
  - The proof of concept's target was at least 50 KB with Bluetooth,
    decoding and the UI running.
- **Flash:** the app is 4.16 MB, 69 % of its 6 MB slot. `flash_guard`
  warns at 80 % and fails at 90 %.

### 2.3 What swap mode would reuse (READ)

| Need | Already there |
|---|---|
| Entering and leaving by a restart, the orderly way | The CPU speed's restart, `MainUiHost::setCpuSpeed()` and `stepCpuRestart()` (src/main.cpp): the player paused, the queue flushed, the headphones let go and the amp off, never while the library is written. `PowerSettings::noteRestart()` leaves the next boot a toast |
| A list of the players nearby, with their signal | The Pair screen (Output > Pair new headphones; `OutputPage`, `PairSearch`) |
| B's question, over any page | `Ui::headphonesLost()`'s own dialog (src/ui/Ui.cpp) on `Dialog` (src/ui/Overlays.h): two buttons, a live status line, a buzz |
| "Send to ..." on a track or an album | The Library's long-press `Sheet` (src/ui/LibraryPage.cpp), at its 4-row cap on a track (section 4.6) |
| Writing a file on the card | `CardFat`'s `FatFile` (src/storage/CardFat.h): 4 KB pieces that end on the file's 4 KB boundaries (`tagstore::writePiece()`), and `rename()` |
| "Does it fit?" | `CardSpace` (src/storage/CardSpace.h): FatFs's free count, read with no card access, or counted a piece per loop pass when it is unknown |
| Half-received files hidden | The walk skips any name starting with `.` (METADATA.md 2.8.2) |
| Received files in the library | The walk, then the tag scan of files no record covers (METADATA.md 1.2, 3.2.3) |
| A whole file's check | CRC-32 (METADATA.md 2.3.2) |
| The texts | Ten languages (i18n/README.md), and the width tests |

---

## 3. The radio options

| Option | What it is (DOCUMENTED) | Verdict |
|---|---|---|
| ESP-NOW | Connectionless vendor frames on the WiFi radio in STA mode: no access point, no IP. v2 frames of 1,470 B (IDF 5.4 and later). A unicast frame is acknowledged by the MAC. CCMP encryption per peer, broadcast in clear. Rate 1 Mbps by default, settable per peer | **Chosen** |
| SoftAP and TCP | One player is an access point and the other joins it. TCP 14.6 TX and 21.1 RX Mbit/s in a shield box with the WiFi and lwIP IRAM options off and the fewest buffers (ESP32 with PSRAM). Needs lwIP, DHCP and an access point's password | Maybe later, as a bulk path (section 7) |
| Both on a home network, with mDNS | Needs the network's password on both players, and isn't "nearby" | No |
| Wi-Fi Aware (NAN) | Standalone only: no STA or AP beside it, so no ESP-NOW either. Its data path is unencrypted, and it runs IPv6 over lwIP. Not in Arduino's build | No |
| Wi-Fi Direct (P2P) | Not in ESP-IDF | No |
| Bluetooth LE to find, WiFi to send | A second stack in the same boot, for a list that ESP-NOW's beacons already give | No |

### 3.1 What ESP-NOW gives, and what is ours (DOCUMENTED)

- **Frames:** 1,470 B of payload in v2, 250 B in v1. A v2 device
  receives both. Both players run the same firmware, so v2 everywhere.
- **Rate:** 1 Mbps by default. `esp_now_set_peer_rate_config()` sets it
  per peer (`esp_wifi_config_espnow_rate()` is deprecated).
- **Delivery:** the MAC acknowledges a unicast frame, and the send
  callback says whether it arrived. Order, retries beyond the MAC's, and
  putting a file back together are ours.
- **Pacing:** send the next frame once the last one's send callback has
  returned.
- **Peers:** at most 20, of which at most 17 are encrypted (7 by
  default). A link needs one.
- **Encryption:** CCMP with a key per peer (the LMK). Broadcast can't be
  encrypted: "Encrypting multicast vendor-specific action frame is not
  supported." So the beacons go in clear.

---

## 4. The design (PROPOSED)

### 4.1 Its own boot

- **In:** Output > Swap with a friend.
  1. A dialog: Bluetooth goes off and the player restarts; the
     headphones come back on leaving.
  2. The CPU speed's orderly restart, with a flag in RTC memory, not
     NVS, as the spike's `Eb` does.
  3. The boot that finds the flag clears it. Any later restart, crash
     or power cut comes back as a normal boot.
- **The boot:**
  - Bluetooth never starts. Its controller's reserved memory could also
    go to the heap, since that boot never uses it (not measured, and not
    needed).
  - The card, the library and the UI start as in a normal boot.
  - No playback (U2).
  - ESP-NOW comes up on one fixed channel.
- **Out:** Done, or by itself after 10 minutes without a link or on a
  low battery (U6). Either way it restarts into the normal boot.

### 4.2 Finding each other

- **The beacon:** broadcast about once a second, with some jitter.
  - It carries the protocol's version, the player's id (random, made
    once and kept in NVS), its name, and whether it is free.
  - Nothing else: a beacon is in clear, readable by anyone in range in
    swap mode.
- **The list:**
  - as the Pair screen's: each player's name and four signal bars, from
    the frame's RSSI;
  - the strongest first;
  - a player drops off after 5 s unheard.
- **Names:** the player has no keyboard. A default from the id
  ("mStream Player 3F2A"), and a name a computer can set over the `@`
  framing with a new verb (USB-VISUALIZER.md). U1.

### 4.3 The request and the approval

1. **A to B:** REQUEST, carrying A's id and name, an X25519 public key
   and a nonce.
2. **B to A,** at once and without its listener: its own key and nonce,
   or BUSY.
3. **Both** work out the shared secret and the code: 6 digits of a hash
   over both keys and both nonces.
4. **The screens:**
   - A shows "Waiting for <B> · 482 913 · Cancel".
   - B shows a dialog over any page, with a buzz: "<A> wants to swap.
     Check that both screens say 482 913." Decline / Accept.
5. **Accept:** each player adds the other as an encrypted peer, with the
   key derived from the secret (HKDF). B's ACCEPT is the link's first
   encrypted frame.
6. **No link** after Decline, Cancel or 30 s.
7. **Limits:** one request at a time. A player that B declined is
   ignored for 2 minutes.

Matching codes mean no third player sits between the two; the comparison
is the listeners' to make. Once linked, every message is encrypted with
the link's key. Section 9 checks on the device that a frame from the
peer's address without that key is refused. mbedTLS is in the
framework, and the WiFi driver's WPA code already links part of it
(libmbedcrypto, section 5.2).

### 4.4 Sending

- **What:** a track, an album, an artist or a folder, from the Library's
  long-press sheet. That means its audio files (`.mp3`, `.flac`,
  `.opus`) and the album's cover image (`.jpg`, `.jpeg`).
- **The offer:** OFFER carries a file id, the size, the CRC-32 and the
  path below `/music`. The receiver answers TAKE, or REFUSE with the
  reason:
  - a type the walk doesn't list;
  - no room. `CardSpace`'s free count decides. When the card doesn't
    know its count, swap mode's boot counts it, a piece per loop pass,
    since nothing plays then.
  - a path that breaks the card's rules: over 255 bytes, more than 8
    folder levels, a name FAT can't hold, or `..`;
  - a file that is already there (same size and CRC: "already here").
    The same name with other content gets " (2)".
- **The data:**
  - DATA frames carry an offset and up to about 1,400 B.
  - A window of frames stays in flight: the spike showed that the
    send's turnaround, not the airtime, limits one frame at a time.
  - The receiver acknowledges what it has, and the sender resends what's
    missing.
- **On the card** (the settings section 7.2 measured):
  1. The receiver reserves the file's room with `f_expand` (one run of
     clusters, no FAT writes mid-file). On a card with no such run, it
     writes without.
  2. It gathers whole 32 KB pieces (a cluster) and writes them to
     `/music/.swap/<id>.part`, with one sync at the end.
  3. At the end it checks the CRC-32.
  4. Then `rename()` to `/music/Received/<sender>/<the sender's path
     below /music>`.
- **Cancel and loss:** a Cancel on either side deletes the `.part`, and
  so does a link lost mid-file. Resuming could come later.
- **Both ways at once:** one file at a time in each direction; the rest
  wait in a queue.

### 4.5 After swap mode: the library and the card contract

- **The library:** leaving restarts the player.
  1. The normal boot's walk sees the changed folders by their digests.
  2. The scan reads the new files.
  3. The update step builds the index (METADATA.md 3.2.3, 3.4.2).

  A note leaves that boot a toast ("12 tracks received"), as
  `noteRestart()` does.
- **Auto DJ:** the received tracks aren't in the card's AutoDJ index
  until the next transfer from mStream.
- **The card contract changes.** METADATA.md 2.11 says: "The device
  never writes or deletes anything under `/music`."
  - Swap mode makes the device a writer of `/music/Received/**` and
    `/music/.swap/**`.
  - To the transfer software they are the listener's files (read and
    list only), so mstream-terminal never deletes them.
  - One owner per file (METADATA.md 1.3) still holds.
  - 2.8 and 2.11 need the change before swap mode is built.

### 4.6 The UI (PROPOSED)

- **Output:** a "Swap with a friend >" row and its dialog.
- **In swap mode:**
  - the Swap page: the players nearby, like Pair;
  - A's waiting card, and B's dialog;
  - once linked, a card with the other player's name, what goes each
    way ("Receiving One More Time · 38 %", in the card's status line),
    and Done.
- **The long-press sheet:** it is at its 4 rows on a track (Auto DJ from
  here, Play, Play next, + Queue). Nothing plays in swap mode, so
  "Send to <B>" takes its first row and the rows that play go.
- **Toasts:** "Sent: <name>", "Received: <name>", "Not enough room on
  the card".
- **Languages:** every text in all ten (i18n/README.md, and the width
  tests).

---

## 5. The link spike (MEASURED, 2026-10-19)

### 5.1 What was built

- **`[env:core2-espnow]`:** core2 plus `-DMSTREAM_ESPNOW_SPIKE=1`.
- **`src/spike/EspNowSpike.{h,cpp}`** (about 370 lines), reached from
  the console's `E`:
  - **`Ei`** brings it up: `esp_event_loop_create_default()`,
    `esp_wifi_init()` (WiFi's NVS off), STA, `esp_wifi_start()`, channel
    1, `esp_now_init()`. It logs the internal heap and PSRAM at each
    step. It calls the IDF directly, not WiFi.h's `WiFi.mode()`, which
    brings esp_netif and lwIP.
  - **`Ep<mac>`** sets the peer.
  - **`Er<1|11|24|54|m7>`** sets its rate with
    `esp_now_set_peer_rate_config()` (54 Mbps by default here).
  - **`Et<n>`** sends n frames of 1,470 B, each after the last one's send
    callback (100 ms at most).
  - **`Ez`** zeroes the receive counters, and **`Ex`** takes it all down
    again.
  - **`Eb`** restarts into a boot that never starts Bluetooth, the card
    or the UI. An RTC flag, then the firmware's own restart; `setup()`
    stops after the boot's `[heap]` line, with only `E` working.
- **Where it is:** not in the tree. It was left uncommitted on its work
  branch, `claude/ecstatic-bose-7e4267`. The list above is what it does.
- **The toolchain:** PlatformIO Core 6.2.0, on Python 3.12 (pioarduino
  refuses anything older than 3.10). Platform 55.03.312-1 (Arduino
  3.3.12 on IDF 5.5.5), as platformio.ini pins.

### 5.2 The build

| | core2 (main, b78002e) | core2-espnow | Change |
|---|---|---|---|
| IRAM used, of 131,072 B | 125,911 | 128,587 | **+2,676** |
| IRAM free | 5,161 | **2,485** | |
| Static internal RAM (`.dram0.data` + `.dram0.bss`) | 57,680 (24,600 + 33,080) | 73,492 (28,676 + 44,816) | **+15,812** |
| `.flash.text` | 1,838,180 | 2,174,756 | +336,576 |
| `.flash.rodata` | 2,280,424 | 2,311,280 | +30,856 |
| `firmware.bin` | 4,358,576 (4.16 MB, 69 %) | 4,733,008 (4.51 MB, 75 %) | **+374,432** |

`iram_diet` (51 of 51), `cache_guard` and `flash_guard` all pass on the
spike's build.

**Where the IRAM comes from** (the map's input sections: +2,600 B, plus
76 B of alignment):

| Archive | IRAM (B) | What |
|---|---|---|
| libpp.a | +1,947 | `.iram1`: wdev.o 979 (the MAC's interrupt and DMA path), pp.o 350, hal_mac.o 184, pm_coex.o 164, lmac.o 140, hal_mac_rx.o 107, pp_debug.o 23 |
| libesp_wifi.a | +330 | esp_adapter.c's `IRAM_ATTR` wrappers: the allocators, the ISR queue and semaphore calls, the coexistence hooks |
| libesp_hw_support.a | +108 | `wifi_module_enable/disable` (sections.ld puts them in IRAM) |
| libnewlib.a | +107 | `__atomic_fetch_sub_4`, for wpa_supplicant's eloop.c |
| libesp_timer.a | +97 | ets_timer_legacy.c, the blobs' timers |
| libesp_phy, libesp_coex, libphy | +16, +7, -12 | |
| libnet80211, libespnow, libcoexist, librtc, libwpa_supplicant | 0 | Their optional IRAM code goes to flash; Bluetooth links coexist, phy and rtc already |

**Why so little** (READ, measured on the map):

- Arduino's prebuilt libraries are built with these WiFi options off
  (the framework package's `esp32/sdkconfig`):
  `CONFIG_ESP_WIFI_IRAM_OPT`, `CONFIG_ESP_WIFI_RX_IRAM_OPT`,
  `CONFIG_ESP_WIFI_SLP_IRAM_OPT` and `CONFIG_ESP_WIFI_EXTRA_IRAM_OPT`.
  `CONFIG_LWIP_IRAM_OPTIMIZATION` is off too.
- So their sections.ld sends `.wifi0iram`, `.wifirxiram`, `.wifislp*`
  and `.wifiextrairam` to flash: 38,348 B of them land there.
- With the IDF's own defaults, about 32 KB of WiFi code would have gone
  to IRAM, which this firmware doesn't have.
- The cost is WiFi throughput: the IDF documents the options as speed
  for IRAM.

**No IP stack:**

- The spike's image has 0 B from liblwip.a and libesp_netif.a, and no
  `lwip_`, `tcpip_`, `esp_netif_`, `netif_` or `pbuf_` symbols.
- The map lists lwIP's sntp and DHCP members in both builds, but the
  linker drops their sections.

**Static internal RAM** (+15,703 B in sections, plus 109 B of
alignment):

| Archive | Bytes | Largest pieces |
|---|---|---|
| libnet80211 | +9,414 | `g_cnxMgr` 3,880, `s_wifi_nvs` 1,308, `g_ic` 708, `gChmCxt` 592, and NAN's `s_dp` 548 and `s_ni` 344, though NAN is off |
| libpp | +4,183 | `TxRxCxt` 972, `g_pm` 544 |
| libwpa_supplicant | +1,371 | `gWpaSm` 828 |
| libesp_wifi | +490 | |
| the rest | +245 | mbedcrypto, espnow, the spike, coexist |

Of the +15,812 B, 11,736 B is `.bss` (section 8, item 1).

**Flash** (about +374 KB):

| Library | KB |
|---|---|
| libnet80211 | +135.7 |
| libmbedcrypto (WPA's crypto) | +71.0 |
| libpp | +64.6 |
| libwpa_supplicant | +63.6 |
| merged strings | +11 |
| libespnow | +5.9 |
| libesp_wifi | +2.6 |
| the spike | +2.4 |

### 5.3 On the Core2

One Core2 (ESP32-D0WDQ6-V3, rev 3.1), with an SD card in, headphones
remembered and the touch calibrated. Before the tests it ran the v0.11.0
release.

- **Uploads** ran at 460800 baud, set in a gitignored `local*.ini`. On
  the Mac used, platformio.ini's 921600 fails right after "Changing baud
  rate", with nothing written: the Core2's CH9102 USB bridge on macOS's
  own driver.
- **Nothing kept was touched:** no flash erase, the tests never wrote
  NVS (WiFi's own NVS was off), and nothing was written to the card.

**The baseline:** a clean build of main (b78002e, v0.11.0-2-gb78002e)
boots on QIO to the UI. Its `[heap]` lines say boot 185K, audio 82K, ui
78K (largest block 71K).

**The spike's normal boot, with Bluetooth:**

- Its `[heap]` lines say boot 167K, ui 61K: the extra static RAM shows.
- `Ei` came up in 33 ms, with modem sleep forced.
- `[stats]` min dipped to 12K during the bursts.
- `Ex` (WiFi down again) leaves the event loop's 6 KB behind, and splits
  the largest block from 57 KB to 22.5 KB.

| `Ei` step | Internal free (B) | Largest block (B) | PSRAM free (B) |
|---|---|---|---|
| before | 62,660 | 57,332 | 2,589,800 |
| after `esp_event_loop_create_default` | 56,620 | 53,236 | |
| after `esp_wifi_init` | 23,808 | 21,492 | 2,585,316 |
| after `esp_wifi_start` | 20,672 | 18,420 | |
| after `esp_now_init` | 20,564 | 17,396 | |
| after `Ex` | 54,728 | 22,516 | 2,589,800 |

**The `Eb` boot, without Bluetooth** (swap mode's case, but without the
card and the UI):

- `Ei` came up in 118 ms. The RF calibration happens here; in the other
  boot, Bluetooth had already done it.
- The real swap boot also starts the card, the library and the UI, so
  some of the 130 KB goes to them (not measured).

| `Ei` step | Internal free (B) | Largest block (B) | PSRAM free (B) |
|---|---|---|---|
| before | 171,016 | 110,580 | 4,187,592 |
| after `esp_event_loop_create_default` | 164,984 | 110,580 | |
| after `esp_wifi_init` | 132,084 | 110,580 | 4,183,108 |
| after `esp_wifi_start` | 129,900 | 110,580 | |
| after `esp_now_init` | 129,792 | 110,580 | |
| after `Ex` | 163,508 | 110,580 | 4,187,592 |

**Sending only:** to the broadcast peer (ff:ff:ff:ff:ff:ff), frames of
1,470 B, each sent after the last one's send callback. The driver's
rate code confirmed each rate, and no frame failed, timed out or was
refused.

| Rate | With Bluetooth | Without (`Eb`) |
|---|---|---|
| 54 Mbps 11g | 500 in 275 ms: 21.4 Mbit/s, 547 µs a callback (498-4,986) | 1,000 in 520 ms: **22.6 Mbit/s**, 517 µs (476-3,522) |
| HT20 MCS7, short GI | 500 in 639 ms: 9.2 Mbit/s, 1,276 µs (456-60,932) | 1,000 in 474 ms: **24.8 Mbit/s**, 471 µs (436-2,343) |
| 24 Mbps 11g | | 1,000 in 805 ms: 14.6 Mbit/s, 802 µs (766-5,364) |
| 1 Mbps 11b (ESP-NOW's default) | 100 in 1,871 ms: 0.63 Mbit/s, 18.7 ms | 100 in 1,287 ms: 0.91 Mbit/s, 12.9 ms |

At 54 and 72 Mbps a frame's airtime is about 0.2 ms. Most of the
roughly 0.45-0.5 ms per frame is the turnaround from send to callback,
so two frames in flight should beat one.

**Afterwards:** a clean build of main again (v0.11.0-2-gb78002e). It
boots to the UI with its settings as before: NVS schema 4, the
language, 240 MHz, the touch calibration, the queue and the screen
settings. The unit now runs main's tip, two commits past the release:
docs and the version string only.

### 5.4 What the spike didn't measure

- **A transfer between two Core2s:** unicast with the MAC's
  acknowledgements, encrypted, with the card's writes.
  - REPORTED (DevKitC boards, 250 B frames, indoors): unicast ran at 81
    % of broadcast at 54 Mbps, 778,500 against 963,000 B/s.
- **Range:** REPORTED, outdoors at about 20 m: 24 Mbps did best (549,250
  B/s), and 54 Mbps lost almost every frame.
- **The swap boot with the card, the library and the UI up:** the `Eb`
  boot had none of them.
- **The battery.**

---

## 6. IRAM for later

Swap mode's own code runs from flash, so the 2,485 B left are for
whatever comes next. The spike ranked the levers on its map (MEASURED
on the map, unless DOCUMENTED):

| # | Lever | Frees | Cost and caveats |
|---|---|---|---|
| 1 | `iram_diet` emulating the IDF's own FreeRTOS flash list (what `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH` moves), in the build's copy of sections.ld | **9,081 B** (66 linked functions); the ringbuf list adds 1,641 B the same way | No framework rebuild. Needs per-function rules (the prebuilt sections.ld keeps libfreertos in IRAM whole), and a bench of the audio path. DOCUMENTED as safe unless misused from an ISR |
| 2 | `custom_sdkconfig` with `CONFIG_ESP32_REV_MIN_3` | about 2.6 KB here (DOCUMENTED "10 KB or more", but `iram_diet` took the libc part already) | A rebuild of the framework, for core2 and core2-dio alike. `no_psram_fix.py` and `iram_diet`'s IRAM half then go |
| 3 | `custom_sdkconfig` with `CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH` | 6,428 B (all of libheap's IRAM) | Risky here: the WiFi adapter and coexistence keep their allocator wrappers in IRAM, and the option is only safe if no heap call comes from an IRAM ISR with the cache off |
| 4 | `custom_sdkconfig` with `CONFIG_ESP_SYSTEM_ESP32_SRAM1_REGION_AS_IRAM` | up to 40,960 B (memory.ld.in's `SRAM1_IRAM_LEN`), paid from the internal heap only as far as IRAM uses it | Needs the rebuild: libheap's reservation of the SRAM1 alias is compiled in. DOCUMENTED: an app with it needs a bootloader that knows the option. READ: every release so far shipped the IDF 5.5.5 bootloader, whose load check accepts IRAM up to 0x400AA000, so existing units should load such an app. Check that on a device before app-only OTA relies on it |
| 5 | `iram_diet` on WiFi's own placements | about 0.2 KB at most (`wifi_module_enable/disable`, `__atomic_fetch_sub_4`) | libpp's 1,947 B is the MAC's interrupt path and must stay |

When the 2,485 B run out, start with lever 1. Lever 4 is the big reserve
if the project moves to `custom_sdkconfig` anyway.

---

## 7. Speed

### 7.1 The radio

- MEASURED: 22.6 Mbit/s send-only at 54 Mbps (section 5.3).
- With acknowledgements, perhaps 80 % of that (the REPORTED ratio):
  about 18 Mbit/s, or 2.2 MB/s (ESTIMATED).
- At ESP-NOW's default 1 Mbps it is 0.91 Mbit/s send-only (MEASURED),
  and 214 kbit/s for unicast in the open (DOCUMENTED). That is about 3
  minutes for a 5 MB file, so the rate must be raised.
- **Range against rate:** step the rate down when frames are lost (54,
  then 24, then 11 Mbps).
- **SoftAP and TCP as a bulk path:** DOCUMENTED at 14.6-21.1 Mbit/s
  with the IRAM options off. That is no faster than the cards, so it
  isn't worth it.

### 7.2 The card (MEASURED, 2026-10-19: the console's `gx` and `gxs`)

The bench (src/app/TagConsole.cpp; the driver's split in lib/SD's
`sd_bench.h`):

- It writes a file in `/.player` once per setting and prints each run's
  MB/s, its pieces' p50, p99 and max, and the driver's split of the
  time.
- Some runs are read back and checked block by block; each 4 KB block's
  first word is its offset, salted per run.
- The file is deleted after each run.

**The card:** the user's SanDisk, 1 TB (CID maker 0x03, "SN01T", CSD
version 2, TRAN_SPEED 25 MHz). FAT32 with 32 KB clusters, its data area
on a 4 MiB boundary, nearly empty.

**The clock** is asked for 25 MHz and runs at 20 MHz. The SPI driver's
divider rounds down to 80 MHz / 4, and the device's own figure says so.

`gx`, 8 MB a run, music stopped, nothing drawn, with the stock CRC16
(2026-10-19; 7.2.1 has the driver's since, about 3 % faster):

| Setting | MB/s | A piece: p50, p99, max (ms) |
|---|---|---|
| 4 KB pieces, plain (today's writers) | 0.99 | 3.71, 5.97, 31.65 |
| 16 KB pieces, plain | 1.37 | 10.81, 13.25, 16.82 |
| 32 KB pieces, plain | 1.47 | 20.47, 24.82, 28.18 |
| 64 KB pieces, plain | 1.47 | 42.06, 50.02, 51.09 |
| 32 KB, `f_expand` (the receive path), read back | 1.47 (read 1.31) | 20.48, 22.82, 25.15 |
| 32 KB, `f_expand`, a sync every MB | 1.47 | 20.48, 23.84, 24.92 |
| 32 KB, `f_expand`, a sync every piece | 1.37 | 21.94, 24.81, 28.13 |
| 32 KB, `f_expand`, no ACMD23 | 1.48 | 20.43, 22.66, 23.26 |
| 32 KB raw CMD25s (no FatFs), read back | 1.47 (read 1.34) | 20.43, 22.58, 24.96 |

Every block read back as written, with 0 CRC errors and 0 retries. A
64 MB raw run at the same settings (`gxs`) gave 1.48 MB/s writing and
1.34 MB/s reading.

**Where a write's time goes**, the driver's split:

| Piece | The wire | CRC16 | Each CMD25's end | The wait before each block | ACMD23, CMD13, FatFs, the rest |
|---|---|---|---|---|---|
| 4 KB | 47 % | 13 % | 26 % (the longest 19.3 ms) | 11 % | 3 % |
| 32 KB | 69 % | 19 % | 8 % (the longest 3.1 ms) | 3 % | 1 % |

What it says:

1. **32 KB pieces are worth 49 %** over today's 4 KB. A 4 KB CMD25
   pays the card's programming at its end every 8 blocks.
2. **64 KB gains nothing.** FatFs writes at most a cluster per
   `disk_write`, so its CMD25s were 32 KB too.
3. **`f_expand` costs 8 ms per 8 MB** and changed nothing on this nearly
   empty card. Its worth is on a fragmented card (not measured), and in
   writing no FAT mid-file.
4. **Syncs:** one every MB costs nothing; one every piece costs 7 %.
5. **ACMD23 makes no difference** on this card, and **FatFs costs
   about 1 %**: raw CMD25s ran at the same speed.
6. **26.67 MHz (80 / 3) fails** at the first block, both times it was
   tried (`gxs`, `gxs8`). The card's data response 0xE5 ("accepted")
   arrives one bit late, and reads as 0xF2 (0x12 once masked). That is
   the GPIO matrix's delay on MISO (GPIO 38, not an IOMUX pin), as
   Espressif's timing notes predict. 20 MHz is the ceiling with this
   driver.

   Only the bench file's own sectors saw the faster clock: FatFs's reads
   and writes stayed at 20 MHz. The two refused writes were the file's,
   and the file was deleted.
7. **The CRC16 was 19 % of a 32 KB write**, about 60 µs a block. Since
   2026-10-20 the driver works it out two bytes a step (lib/core
   `SdCrc16`, below), and writes went from 1.48 to 1.53 MB/s.

#### 7.2.1 The CRC16 (MEASURED, 2026-10-20: `gxc`, then `gx` with each)

The CRC over each data block is CRC-16/XMODEM. The driver used the
framework's `CRC16()`, a byte a step through a 256-entry table in flash.
lib/core `SdCrc16` does two bytes a step through two 256-entry tables
(slicing by 2), with the same result. test_sd_crc16 checks it against
`CRC16()` itself, a bit-at-a-time reference and the SD spec's 0x7FA1.

`gxc` timed each variant on the Core2 at 240 MHz: a 512 B block from
internal RAM, and 512 B blocks streamed from 64 KB of PSRAM, as the
bench writes them. All gave the same CRCs.

| Variant | From RAM (µs a block, cycles a byte) | From PSRAM (µs a block) |
|---|---|---|
| The stock table, a byte a step (`CRC16()`, the driver's before) | 43.8 (20.5) | 62.8 |
| The ROM's, a byte a step (`esp_rom_crc16_be`) | 35.1 (16.5) | 53.6 |
| Two bytes a step, tables in RAM (`SdCrc16`, the driver's now) | 32.9 (15.4) | 51.4 |
| Two bytes a step, tables in flash | 36.1 (16.9) | 54.6 |

`gx` with the driver on each (`gxc0`, then `gxc1`, in the same boot):

| Setting | Stock CRC16 (MB/s) | Two bytes a step (MB/s) |
|---|---|---|
| 4 KB pieces | 1.01 | 1.02 |
| 16 KB pieces | 1.38 | 1.42 |
| 32 KB pieces (plain, `f_expand`, a sync every MB, no ACMD23) | 1.48 | 1.52-1.53 |
| 32 KB raw CMD25s | 1.49 | 1.53 |
| Read back (32 KB, `f_expand`; raw) | 1.32; 1.35 | 1.36; 1.39 |

Writes and reads both gain about 3 %, and the CRC16's share of a 32 KB
write fell from 19 % to 16 %. Why not more:

1. **About 18 µs of each block's CRC time is PSRAM**, the same for every
   variant: the block's 16 cache lines coming in. Whatever reads the
   block first pays it. Without the CRC, the SPI copy into the FIFO
   would.
2. **The arithmetic is a chain of dependent loads.** Two bytes a step
   still run at about 15 cycles a byte, not the 6-8 a byte the
   instruction count suggests. Slicing by 4 or 8 would add 1-3 KB of
   tables for perhaps 1-2 % more (ESTIMATED).
3. **The tables' place matters little.** RAM against flash is 3 µs a
   block, about 1 % of a write, for 1 KB of static RAM in every boot.
   The ROM's routine costs no RAM and no flash, and is 2 µs a block
   behind the RAM tables. The user kept the tables in RAM (2026-10-20).

**What would gain more: overlapping the CRC with the wire.** The driver
sends a block 64 bytes at a time through the SPI FIFO and waits about
26 µs for each 64 bytes to go out at 20 MHz, doing nothing. Working out
the CRC of the 64 bytes just queued during that wait would hide all of
it. That is up to about 10 % more on a 32 KB write (about 1.7 MB/s,
ESTIMATED). It means a FIFO loop of the driver's own, on the SPI
registers, in place of the HAL's `writeBytes()`: a deeper change, not
made.

### 7.3 End to end (ESTIMATED)

- The sender reads its file from its own card: 1.36-1.39 MB/s MEASURED
  (the bench's read-backs, the fast CRC16), 1.2-1.7 MB/s in
  METADATA.md.
- The radio carries about 2.2 MB/s, and the receiver writes 1.53 MB/s.
- With a ring on each side, so that reading, sending and writing
  overlap, the slowest sets the pace: about 1.35 MB/s. One after
  another, much less.

| File | Time at 1.35 MB/s |
|---|---|
| 5 MB MP3 | about 4 s |
| 30 MB FLAC | about 22 s |
| 100 MB album | about 1.2 minutes |

---

## 8. Costs and risks

1. **Normal mode's internal RAM.**
   - Linking WiFi adds 15,812 B of static RAM to every boot. MEASURED:
     the normal boot's `[heap]` ui line goes from 78K to 61K.
   - The 0.11.0 checks' lowest, 53,012 B, would become about 37 KB
     (ESTIMATED), under the proof of concept's 50 KB target.
   - 11,736 B of it is `.bss`. `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`
     puts the `.bss` of lwIP, net80211, libpp, wpa_supplicant and
     bluedroid in PSRAM (DOCUMENTED), so it would also move some of
     Bluetooth's.
   - That needs the `custom_sdkconfig` rebuild (ENERGY.md, P3b), and
     PSRAM can't be reached while the flash cache is off. A decision
     for any WiFi, not only swap mode.
2. **The battery.**
   - DOCUMENTED: listening draws 95-100 mA at 3.3 V; sending at 54 Mbps,
     190 mA.
   - ESTIMATED: about 2 hours from a full charge (1.44 Wh, ENERGY.md) at
     about 0.65 W.
   - To measure in the swap boot with the console's `P`. Hence the exit
     by itself.
3. **Flash:**
   - 75 % of the slot (MEASURED); `flash_guard` warns at 80 %.
   - I18N.md keeps 0.6-1.0 MB for WiFi, HTTP and TLS, and the driver's
     374 KB is now part of it.
4. **The link:**
   - a send callback that never comes: a timeout per frame, as the
     spike's 100 ms;
   - `ESP_ERR_ESPNOW_NO_MEM`: wait, then retry;
   - a channel busy with someone's WiFi.
5. **Security:**
   - the beacons are in clear (a name and an id);
   - request spam: one request at a time, and the 2-minute ignore;
   - a linked player's files are no more trusted than files copied from
     a PC. The decoders and the tag scan already take any file.
6. **Every check needs two Core2s.**

---

## 9. Next steps

1. **The two-Core2 test** with the spike's commands (`Ei` on both, then
   `Ep`, `Er` and `Et`). It measures:
   - unicast throughput and losses at 11, 24 and 54 Mbps, close and
     across a room;
   - one frame in flight against two;
   - encrypted against not;
   - whether a frame from the peer's address without the key gets
     through.
2. **The decisions** of section 10, and normal mode's internal RAM
   (section 8, item 1).
3. **The CRC16 inside the wire's time** (section 7.2.1): the driver's
   own FIFO loop, working out each 64 bytes' CRC while they go out,
   measured with `gx`. A faster CRC alone is done: about 3 %.
4. **The build, in order:**
   1. the swap boot: in, out, the flag, no Bluetooth, ESP-NOW up;
   2. the protocol in `lib/core`, host-tested with two players on a
      simulated link that loses, repeats and reorders frames: the
      request's states, the code, the window, the path rules;
   3. the receive path on the card: the `.part`, the pieces, the CRC,
      the rename, the room;
   4. the UI and its texts;
   5. the device checks.

   ESTIMATED: 3-5 weeks.

---

## 10. Open questions (for the user)

- **U1, names:** the id's default, a name set from a computer, or both?
- **U2, playback in swap mode:** none, or the speaker, to try a received
  track?
- **U3, approval:** the link only, as described, or each file too?
- **U4, where files land:** `/music/Received/<sender>/...`, or the
  sender's own folders?
- **U5, the code:** compared on both screens, as designed, or no code?
  No code is simpler, but anyone in range could sit between the two.
- **U6, the exit by itself:** 10 minutes without a link? A battery
  floor?
- **U7, order:** swap mode before or after the WiFi groundwork mStream
  sync needs (the rebuild, `.bss` in PSRAM)?

---

## 11. Sources

Espressif (DOCUMENTED):

- [ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-reference/network/esp_now.html)
  (ESP-IDF v5.5): v1 and v2 frames, the default rate, peers, encryption,
  pacing.
- [ESP-NOW FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/application-solution/esp-now.html):
  214 kbit/s in the open and 555 kbit/s in a shield box, at 1 Mbps
  (ESP32-DevKitC V4).
- [RF coexistence](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-guides/coexist.html)
  (v5.5): the table in 2.1.
- [Wi-Fi driver](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-guides/wifi.html)
  (v5.5): the throughput table for ESP32 with PSRAM, the IRAM options'
  sizes, and Wi-Fi Aware's limits.
- [RAM usage](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-guides/performance/ram-usage.html)
  (v5.5): `CONFIG_ESP32_REV_MIN_3`,
  `CONFIG_ESP_SYSTEM_ESP32_SRAM1_REGION_AS_IRAM`, and the FreeRTOS and
  heap flash options.
- [External RAM](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-guides/external-ram.html)
  (v5.5): `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`.
- [ESP32 datasheet](https://documentation.espressif.com/esp32_datasheet_en.pdf):
  RF current consumption.
- [esp32-arduino-lib-builder, configs/defconfig.common](https://github.com/espressif/esp32-arduino-lib-builder/blob/master/configs/defconfig.common):
  `CONFIG_ESP_WIFI_IRAM_OPT` and `CONFIG_ESP_WIFI_RX_IRAM_OPT` not set.

Others (REPORTED):

- [ESP-NOW data transfer](https://blogs.ntu.edu.sg/ps9888-2020-g16/2020/07/11/esp-now-data-transfer/)
  (NTU, 2020): unicast and broadcast at each rate on two ESP32-DevKitC
  boards, 250 B frames, about 1 m indoors and 20 m outdoors.
