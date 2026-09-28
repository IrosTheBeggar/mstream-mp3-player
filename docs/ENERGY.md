# Energy plan

Goal: make the Core2 last as long as it can in Bluetooth mode (Tangara's
number one complaint), and add a sleep timer (its second one), without
breaking the hearing and UX rules: nothing plays out loud unexpectedly,
headphone input never starts music, and a volume never jumps up.

**Where the numbers come from.** Figures marked *measured* were taken on
this Core2 with the power probe (`P` commands, see ARCHITECTURE.md "Power
measurement"). Every one was taken on USB (ACIN at 5.13 V) with the
battery full and idle (it read 0.0 mA), so the ACIN current is the whole
device's draw. Each figure is the mean of 12-13 five-second windows
(60-65 s) after a `Pm` marker. The window sd was 2-4 mA and the baseline
drifted 108.9-111.3 mA over 30 min, so **differences under ~2.5 mA can't
be resolved**. Figures marked *estimate* come from the static audits
(datasheets, forum figures, CPU-share arithmetic) and have not been measured.

Two units are used:

- **USB mA**: what the probe read at 5.13 V.
- **bat mA** (battery-equivalent): W / 3.7 V = USB mA x 1.386. This is an
  approximation: the AXP192's battery path is not the USB path. No
  battery-only run has been done yet (step 9 below).

Battery: 390 mAh x 3.7 V = 1.44 Wh. Hours below use all of it. Multiply
by ~0.9 for a realistic cut-off.

---

## 1. Summary

### Where the power goes while streaming

State: Bluetooth streaming an MP3, Now Playing on screen, backlight 127,
240 MHz, default TX power. Total: **~116 USB mA, 0.60 W, ~161 bat mA,
about 2.4 h** on a full battery.

| Part | USB mA | bat mA | Share | Basis |
|---|---|---|---|---|
| Bluetooth A2DP streaming (radio, SBC encode, the data path) | ~60 | ~83 | 52% | measured: streaming silence 109.8 minus idle with the link up 49.9 |
| Platform floor (ESP32 idle at 160 MHz, PMIC, PSRAM, SD, touch, IMU, LCD logic asleep) | ~30 | ~41 | 26% | measured: idle, screen off, 160 MHz = 28.5 |
| Screen at 127 (backlight ~12.5, panel awake ~2.3) | ~15 | ~21 | 13% | measured: `Ps0` saves 14.8 while streaming |
| MP3 decode | ~6.5 | ~9 | 6% | measured: MP3 vs silence at the same TX, +6.5 |
| CPU at 240 instead of 160 MHz | ~5 | ~7 | 4% | measured: `Pcb160` saves 5.2 |

Extras on top, when they happen (measured):

- **Dance tab:** +4.5 USB mA with the crab idle, +8.8 while dancing (30 fps).
- **Scrolling a list at 30 fps:** +8.3.
- **Speaker amp left on after any speaker use:** +5.3, for the rest of the uptime.
- **Background reconnect cycling while the headphones are gone:** +35.5
  (85.7 against 50.2 with it paused). This is 32% of the streaming
  baseline, on a device that isn't playing anything.

What doesn't cost anything, measured:

- **An idle headphone link (sniff):** 49.9 linked vs 50.2 unlinked. Staying
  connected while paused costs the Core2 nothing, so there is no energy
  reason to drop a paused link early.
- **Each of these knobs:** green LED, 5 V boost (EXTEN), IMU suspend, a 20 ms
  loop delay, the audio taps. Each is below the noise. All four at 160 MHz
  together save 2.5 (screen off) to 3.9 (screen on) USB mA.

**The headline.** Half the listening drain is the A2DP radio itself. That
is inherent to Bluetooth audio, and Espressif's own A2DP figure barely
moves with power management. So listening time can grow by about a
quarter, not double. The large wins are in the states where nothing is
being listened to:

- the reconnect cycle with the headphones gone;
- a lit screen nobody looks at;
- falling asleep with music on;
- a device left on overnight.

That is also where the reported "dead by morning" came from: screen on,
headphones gone, reconnect cycling = 85.7 USB mA, flat in ~3.3 h.

### Battery life, today vs after the plan

| Situation | Today | After the plan | Basis |
|---|---|---|---|
| Listening over BT, MP3, screen on the whole time | 0.60 W, **2.4 h** | 0.55 W, 2.6 h (160 MHz, amp off, small knobs) | measured parts |
| Listening over BT, MP3, screen times out (normal use) | 0.60 W, **2.4 h** (no timeout exists) | ~0.48 W, **~3.0 h** (~3.1 h if the lower TX floor takes effect) | measured: 87.0 screen off + knobs at 160, plus 7.4 MP3 at 160 |
| Paused, headphones linked, left alone | 0.26 W, 5.6 h | 0.13 W for 20 min, then **off** | measured 49.9 today; 24.3 at 80 MHz, screen off |
| Headphones gone, screen on, reconnect cycling (the reported night) | 0.44 W, **3.3 h** | ~0.11 W (13 h) until the idle power-off at 20 min, then off | measured 85.7 today; 20.8-22.9 with all measures |
| Fell asleep, 8 h night, 30 min sleep timer | plays until flat (~2.4 h) | ~30 min streaming + ~20 min idle, then off: **~80% left in the morning** (estimate) | measured parts, off current estimated |
| Powered off | n/a | ~0.26 mA, weeks (estimate: a forum figure for a Core2 with only the AXP192 and RTC on) | not measured |

Net: **+25% listening time**, and the idle and overnight cases go from
"flat in 3 h" to "off with charge left".

---

## 2. Ranked changes

Ranked by measured saving and safety, biggest safe saving first. "Setting"
means a user-visible row on the Output tab's settings (beside Haptics) or
in a sheet.

### 1. Background reconnect: burst, back off, then rest connectable

**Measured saving:** -35.5 USB mA (~49 bat mA, 32% of the streaming
baseline) for as long as the headphones are gone. That is the same at
160 MHz (81.1 vs 45.5).

**Status: in the code (step 1), host-tested, measured on the device
(section 4, "Device run: steps 0-3"): with the headphones gone and the
screen on, 51.1 USB mA backing off and 47.7 resting, against 85.7
(-34.6 / -38.0); 29.2 with the screen off. The checks that need the
headphones answering are still to do.** What follows under "What happened" is the firmware the
measurements were taken on.

**What happened before.** The cycle never stopped (the old
ReconnectPlanner's `kScanForMs`, BtSink.cpp's heartbeat):

- about 3 pages, 10 s apart, each up to the 5.12 s page timeout, then
- ~60 s of back-to-back inquiry, then pages again, forever.

Measured windows: 60-84 USB mA while paging, 80-98 while inquiring. The
inquiry cannot find remembered headphones unless they are in pairing
mode. With nothing remembered, the scan by name runs forever. So does
the Pair screen's scan while it stays open.

**The change, in `ReconnectPlanner` so it stays host-tested:**

1. **Burst.** On a drop, at boot, and on any listener ask, page 3 times
   as today. A listener ask is: Play through PlayGate, Connect or a tap
   on the card, a B hold, or opening the Output tab.
2. **Back-off.** Then one page at a time, with no inquiry, at 30 s, 1,
   2 and 5 min, then every 5 min. As built, the library's auto-reconnect
   stays disarmed outside a pairing and the planner makes every page
   itself: re-arming with 1 retry would still reach the "retries
   exhausted, start discovery" branch, from the DISCONNECTED that
   answers that one page.
3. **Resting.** After 15 min without success, stop paging. Stop at once,
   after the burst, when the screen is off and nothing plays or waits.
   As built, not after a drop while listening: that pause is the drop's
   own, and the listener may still be wearing them (a range drop is what
   the back-off is for), so a lost link gets the whole back-off.
   Stay connectable (page scan, ~1% duty, estimated <1 mA), so headphones
   that are switched on or taken out of the case reconnect by themselves.
   The Powerbeats do that.
4. **No background inquiry while a device is remembered.** Scans by name
   are limited to 2 min after boot or after a listener ask, and only when
   nothing is remembered. The Pair screen's scan stops after 2 min and
   offers "Search again". It also stops when the page is left or the
   screen goes off. While the page stays up with its scan stopped, the
   background search stays held off (the old headphones would link while
   new ones are picked); it starts when the page closes.

**UX impact.** Headphones that page back by themselves (most, and the
user's) are unchanged. Headphones that never page back reconnect only
during the first 15 min, or at once on Play, Connect or a B hold. PlayGate
already pages at once, so a waiting Play is unchanged. The Output card
gets a resting line: "Not connected. They'll reconnect when switched
on." [Connect], instead of "Looking for...". The tab icon is not amber
while resting. A link coming up never starts audio, so hearing safety is
unaffected.

| | |
|---|---|
| **Default** | on |
| **Setting** | no |
| **Risk** | medium: the glue with the library's reconnect state has bitten before; the host model in test_reconnect covers it. The page already on its way must still count as try 1 of a Play's burst (ARCHITECTURE.md "Play while the headphones aren't connected"). |
| **Effort** | S-M, 1.5-2 days |

### 2. Screen policy: dim, then off, and a wake that never acts

**Measured saving** (at 240 MHz, while streaming; the idle saving is similar):

| Change | USB mA saved |
|---|---|
| Off: DCDC3 off plus panel sleep-in | -14.8 streaming (-13.5%), -14 to -17 idle |
| Dim, 127 to 30 | -11.9 |
| 127 to 100 | -3.1 |
| 127 to 60 | -7.3 |
| Backlight 255 instead of 127 | costs +25.9 |

Stopping all UI drawing while off is *estimated* at a further 1-2 mA: the
EQ bars at 4 Hz, the 1 Hz times, spinners, thumbnail jobs. It rises to
the whole Dance cost (4.5-8.8) if the Dance tab was left up.

**Status: in the code (step 2), host-tested, measured idle on the device
(bright 100: 44.0, dim: 35.7, off: 29.2 USB mA); a wake-redraw bug found
and fixed there (section 4, "Device run"). The finger, streaming and
pocket checks are still to do.** What follows under "The change" is the plan; the as-built
notes are under step 2 in section 4.

**The change.** A `ScreenPower` state machine in lib/core (host-tested),
with the states Bright, Dim and Off, driven from the loop. Only it calls
`setBrightness`, `sleep` and `wakeup`, and it does so under `LcdLock`: the
panel shares SPI with the SD card.

- **Timing.** From the last input, the same rule whether playing or idle.
  - The screen goes **Off after the chosen time** (default **30 s**).
  - It dims to **30** for the last 10 s (at 7 s for the 15 s choice).
  - While the player is idle, the idle power-off (item 4) takes over later.
- **Stays lit while:**
  - the calibration or spike screens own the display;
  - a play waits for the headphones (PlayGate Waiting, at most its 20 s backstop);
  - a pairing is under way.
- **What wakes it:** a touch on the glass or the strip, and a PWR short
  press (M5.BtnPWR is already polled). Events that need the listener also
  wake it:
  - the headphones-lost dialog;
  - "Couldn't reach";
  - a track that failed;
  - USB plugged in or out;
  - the low-battery warning.

  Headphone keys, track changes and a link coming up do not wake it.
- **The waking touch is swallowed** until it lifts, as `Ps0` did.
  It makes no tap, click, hold, volume repeat, swipe or haptic. This
  applies from **Dim** as well as from **Off**, as one rule: a touch on a
  screen that isn't at full brightness only brightens it. This is a
  hearing-safety requirement: a pocket press of B with the speaker as the
  output must not start music. The scripted finger (`uit`/`uis`) bypasses
  the latch.
- **Pocket guard.** A wake from Off with no further input goes back to Off
  after 10 s, without the dim step.
- **Pocket rule** (as built, from the review). The guard alone let the
  second pocket contact on B, 300 ms after the swallowed one, play out
  loud. After any wake from Off (a touch, PWR, an event, keepLit) and
  until a touch lands on the glass (or PWR while lit, or the console), a
  B click that would start the **speaker** is refused: the inert buzz,
  and the note "Tap the screen first, then B plays". Pausing, A and C,
  the volume, the B hold and a play on the headphones still act, so a
  blind pause from a pocket still works. An event's wake from Off (the
  lost dialog) also swallows the first touch, as a wake from Off does, so
  a pocket contact can't tap "Use speaker".
- **While off:**
  - `Ui::loop` keeps its logic (snapshot, dialogs, toasts, timers) but
    draws nothing;
  - any fling stops;
  - no new thumbnail jobs start;
  - DanceMode is `setActive(false)`;
  - `idleMs()` returns 20 ms.

  On wake it does one full redraw (`tabBar_.invalidate()` plus the page's
  repaint). Allow 120 ms between SLPIN and SLPOUT.
- **Logging.** Every swallowed wake touch logs `[screen] wake by touch at
  x,y (raw)`. The probe run saw one wake with no touch sent. If phantom
  panel events show up, require the wake touch to last ~30 ms.

**Settings** (Output tab, NVS):

- **Screen off after:** 15 s / **30 s** / 1 min / 2 min / 5 min / Never.
- **Brightness:** Low 60 / **Medium 100** / High 160 / Max 255. Pick the
  default by eye indoors. 100 saves 3.1 mA over today's 127. Max costs
  +26 mA, which is the point of letting the listener choose.
- The Dim level is fixed at 30 (the probe's `Pb30`: check it is readable
  enough to show that the screen is about to go off).

**UX impact.** It behaves like a phone: music plays on with the screen
dark, and the first touch only wakes it. Glancing at the time needs a tap
or a PWR press.

| | |
|---|---|
| **Risk** | medium: the SPI lock around sleep and wake; the hardware-scroll band after a wake mid-list (check on the device); dialogs that open while off must still be there on wake. |
| **Effort** | M, 2-3 days with the settings rows and host tests |

### 3. Sleep timer (the requested feature)

**Measured saving:** once it fires, streaming stops: ~-60 USB mA. Then
the screen goes off (-15), and then comes the idle power-off (item 4).
Chained, it turns "plays until flat" into "~80% left in the morning"
(estimate). The full spec is in section 3.

| | |
|---|---|
| **Default** | off, chosen per night |
| **Setting** | it is one |
| **Risk** | low to medium: the hearing-safety ordering of the fade |
| **Effort** | M, 2-4 days |

### 4. Idle power-off

**Saving.** Measured: the best idle state left on (screen off, reconnect
resting, 80 MHz, the small knobs) is still 20.8-22.9 USB mA, 0.11 W, ~13 h.
Off through the AXP192 is *estimated* at ~0.26 mA, a forum figure for a
Core2 with only the AXP192 and RTC powered. Only this survives a night
or a week in a bag.

**The change.** An `IdlePolicy` in lib/core (host-tested). It powers the
device off after **N minutes idle**, where idle means all of these:

- the player is Stopped or Paused, not Waiting;
- not on USB (no ACIN);
- no touch, strip or PWR input, and no headphone key;
- no Pair screen scan or pairing;
- no queue write pending.

Default **20 min**. The sequence:

1. `QueueStore::flushNow()`: a new synchronous finish of any piece-wise
   write, plus the position.
2. Save a "turned off after N min idle" flag.
3. Release the headphones the way the sleep timer does (section 3).
4. Haptics off, `Serial.flush()`, `M5.Power.powerOff()`.

It shows no warning and does not light the screen: it may be night. At
the next boot a toast says "Turned off after 20 minutes idle". PWR boots
the device, stopped, where it was, as today. Resuming inside the track is
already on the roadmap and fits here.

**On USB** it doesn't power off (it can't: ACIN would power it back on,
to be verified). The screen goes off and the reconnect rests, which is
enough on a charger.

Deep sleep with a touch wake is the alternative: *estimated* 1.8-2.6 mA,
and it still cold-boots on wake. Power-off is the better default.

**UX impact.** The next use costs a PWR press and a ~3 s boot, plus the
Bluetooth stack's ~10 s start-up. The toast explains why.

| | |
|---|---|
| **Setting** | "Turn off when idle": 10 / **20** / 60 min / Never |
| **Risk** | low for audio (it only acts when nothing plays, and boots stopped); medium for data (the flush must complete). Verify that a PEK press boots it after `Axp192.powerOff()`, and what ACIN does to an off device. |
| **Effort** | S-M, 1-1.5 days |

### 5. Speaker amp and I2S off when the speaker is quiet

**Measured saving:** -5.3 USB mA (-4.8%). Today, after any speaker use
since boot, the NS4168 enable and I2S stay on until reboot, including
through all later Bluetooth listening.

**The change.** Make the `Pa0` path automatic. The first step is the
power saving; the rest is the timing:

- **Off:** once the speaker is not the consumer, or is paused, and
  M5.Speaker has been idle for more than 2 s, drop AXP192 GPIO2 and stop
  I2S. Both happen on the speaker's own task, as `Pa0` does now.
- **On again:** the next speaker playback turns it on (already so), and
  the existing fade-in covers the start.

It is also a hearing-safety plus: with the amp off, no stray buffer can
reach the speaker while Bluetooth is the output. EXTEN turned out not to
feed the amp: its current was unchanged with EXTEN off.

**UX impact:** none if there is no pop.

**Status: in the code (step 3), host-tested (the AmpGate cases in
test_output_chain); measured on the device: off 2.01 s after a pause,
-5.2 USB mA (45.5 vs 50.7 held on). The by-ear checks are still to do.**

| | |
|---|---|
| **Default** | on |
| **Setting** | no |
| **Risk** | low-medium: an enable or disable pop, not yet checked by ear (the probe ran in silent mode) |
| **Effort** | S, half a day |

### 6. CPU at 160 MHz from boot, and 80 MHz while idle

**Measured saving:**

- **160 MHz:** -5.2 USB mA streaming with the screen on, -5.5 with it off,
  -4.7 idle.
- **80 MHz while idle** (from 160, screen off): -4.2 more.

**The cost at 160, measured:**

- MP3 decode rises from 37.5% to 49.4% of a core, with 0 underruns in
  66 s on the speaker and 45 s over Bluetooth.
- The Dance tab drops from 29-31 to 20-24 fps with an MP3 over Bluetooth.
- List scrolling will be slower too (not yet measured at 160).

**The change:**

1. **Fixed 160 from boot** (what `Pcb160` does, as the default), before
   Bluetooth starts. 240 can't be switched to or from at runtime while
   Bluetooth runs: it retunes the 480/320 MHz PLL the radio uses.
2. **A small governor, 160 to 80,** on the same PLL. It drops to 80 only
   while all of these hold:
   - Stopped, Paused or Waiting;
   - the A2DP stream suspended;
   - no list moving;
   - Dance not shown;
   - no thumbnail job;
   - no finger down.

   It returns to 160 on a touch-down, a play, the Dance tab or a bench.
   The probe already refuses 80 while audio runs and returns to 160 by
   itself.
3. **Later, with the WiFi/IRAM `custom_sdkconfig` rebuild:** `CONFIG_PM_ENABLE`
   with esp_pm DFS (max 160, min 80) and PM locks around decode, UI and
   dance frames. *Estimated* 2-5 mA more, high effort. Automatic light
   sleep is not reachable with Bluetooth on: the Core2 has no 32 kHz
   crystal.

**Default:** 160 once the soak passes. `Pcb240` stays as a console
override. No UI setting, since it needs a reboot.

| | |
|---|---|
| **Risk** | medium: UI smoothness (dance, lists, the stall at a track start); the governor's switches with a Bluetooth link up (soak it) |
| **Effort** | S for 160 (mostly measuring); S-M for the governor; L for DFS |

### 7. Bluetooth TX power: lower the floor, keep the ceiling

**Measured**, each after a reconnect (a link that is already up keeps its
level):

- fixed -12 dBm: -3.7 USB mA, with 0 underruns and a 26 ms max gap over
  65 s (distance not controlled);
- fixed +9 dBm: +2.7.

The default is 0..+3 dBm.

**The change:** `esp_bredr_tx_power_set(N12, P3)`, called after the
controller is enabled and before any page. The ceiling, and so the range,
stays as today, and the controller can follow the headphones' power-down
requests when they are close. The saving lies between 0 and 3.7 mA,
depending on whether the headphones use power control. Never raise the
ceiling to +9 as Tangara did without a dropout problem to fix. A fixed
low ceiling is at most a later opt-in "battery saver", after a pocket
and arm's-length dropout soak.

| | |
|---|---|
| **Default** | on |
| **Setting** | no |
| **Risk** | low with the ceiling unchanged; watch underruns and `gap=` at range |
| **Effort** | XS plus a soak |

### 8. Dance tab frame rate

**Measured cost:** +4.5 USB mA with the crab idle at 30 fps, +8.8 while
dancing.

**The change:**

- **Idle crab** (paused, stopped, no beat lock, pose weight ≤ 0.5): 10 fps.
  *Estimated* -3 mA; breathing and blinking still read fine.
- **Dancing:** 30 fps at 240 MHz; at 160 it already lands at 20-24, so aim
  for a steady 24.
- **Screen off:** stopped entirely (item 2).
- Reset `lastFrameMs_` when the period changes, so there is no catch-up burst.

**Default:** on, no setting.

| | |
|---|---|
| **Risk** | low, visual only |
| **Effort** | XS-S |

### 9. The small peripherals (each below the noise; together -2.5 to -3.9 USB mA)

- **IMU:** suspend the BMI270 at boot (`Pi0`). Nothing reads it. Streaming
  showed -3.3 once, idle ~0; treat it as ≤1 mA. **Do it**: it costs nothing.
- **5 V boost:** EXTEN off at boot (`cfg.output_power = false`). The amp
  isn't fed from it (measured). **Do it** after an ear check that the
  speaker still plays on battery with EXTEN off. Turn it on only for a
  future M-Bus or Grove module.
- **Audio taps:** write the outputs' taps only while the Dance tab is
  shown (`Pk0`, -1.5, noise). This is the "dance tracker off-screen"
  item. The tracker itself is already gated to the tab; only the two
  tap copies ran all the time. **Do it**: no UX change.
- **Loop delay:** 20 ms only while the screen is off (part of item 2).
  Measured as noise while lit, so leave the lit UI at ~5 ms.
- **Green LED:** already off. Keep it off, and don't use it as a
  "playing" light.

**Status: in the code (step 3): the IMU suspended and EXTEN off from boot,
the taps only while the Dance tab is up (test_audio_tap), the 20 ms loop
delay while the screen is off (since step 2). Checked on the device:
the boot line and `imu=suspended exten=off taps=off`, taps on only with
the Dance tab up. The battery ear check for EXTEN is still to do.**

### 10. Measured or reasoned as not worth doing now

- **Dropping an idle, paused link early for the Core2's sake:** the link
  costs ~0 (measured). The headphones are still released after a sleep
  timer and before power-off, for their battery and so that their keys
  go quiet.
- **SBC bitpool:** fixed by the prebuilt stack; needs a rebuild and costs
  audio quality.
- **SD read bursts, the DCDC1 trim and the LCD SPI clock:** estimated
  under 1-3 mA each, with real risk.
- **An event-driven main loop and turning off the PWR-key polling:**
  noise-level (the `Pd20` result), and PWR is now a wake key.
- **A lower FreeRTOS tick:** the code relies on `vTaskDelay(1)` being 1 ms.

---

## 3. Sleep timer spec

### Choices

15, 30, 45, 60 or 90 min; **End of track**; **End of album** (pause when
the next queue entry belongs to a different album; with no album
information, the folder); **End of queue**.

- The choice is never saved: RAM only, so a reboot or power-off clears it.
- Nothing about it is persisted, because a device that reboots stopped
  has nothing to time.

### Where it lives

**Now Playing's "..." sheet** gets a first row, "Sleep timer", with its
state dim on the right: "Off", "23 min" or "End of track". A tap opens
the **Sleep timer sheet**:

- a grid of pills in the sheet panel (320 x 168): "15", "30", "45",
  "60", "90 min" on one row, "End of track", "End of album", "End of
  queue" below;
- while a timer runs, a bottom row with "+10 min" and "Turn off" (red);
- the running choice outlined in the Now Playing accent.

The pill texts go in `UiText.h` with their room, measured by
test_ui_* as the other fixed texts are.

**Indicator:**

- On **Now Playing**, a small moon icon and "23 min" (or "track",
  "album", "queue") on the progress line, taking the place of
  "4 of 16 · SPYDRONE" when both don't fit. In the last minute it
  counts seconds; during the fade it reads "fading".
- On the **tab bar**, a 7 x 7 moon badge in the corner of the Now
  Playing cell, drawn like the Queue's badge (a dirty-cell redraw once a
  minute, no animation).

A tab or page doesn't hide the timer's state for long: the "..." row
always shows it.

### Extend and cancel

| When | What the listener does | What happens |
|---|---|---|
| Timer running | picks another duration | restarts the timer from now |
| Timer running | "+10 min" | adds 10 min to what is left |
| Timer running | "Turn off" | cancels it |
| During the fade | any touch or strip press | wakes the screen, swallowed as usual, and shows a toast: "Sleep timer: fading" with **+10 min** and **Turn off** |

Only the toast's buttons act on the timer. A tap elsewhere doesn't
cancel it: a stray touch shouldn't undo a timer the listener set before
falling asleep. Either button ramps the fade factor back up at
GainRamp's slow rise (≤20 dB/s, the same ~2.4 s it takes after a probe),
never in one step.

### At expiry

1. **Fade over 30 s.** An extra fade factor goes after the gain stage:
   - on Bluetooth, in `BtSink::onData` after `gain_.process`;
   - on the speaker, in `SpeakerSink::pump` before `playRaw`.

   The factor is an atomic Q15 word, never above 1.0. It falls on an
   equal-loudness curve: linear in dB to -40 dB over the 30 s, then to 0.
   Both outputs apply it, so an output switch during the fade carries it.

   For "End of track" and "End of album", the fade covers the last 10 s
   of the track when its length is known (TrackProgress). Otherwise
   there is no fade, and the player pauses at the boundary.
2. **Pause, never stop.** The queue and the position are kept.
   - Timed choices pause where the fade ends.
   - "End of track" and "End of album" need a new `PlaybackController`
     "pause after this track": at the boundary it moves to the next entry
     and stays paused at 0:00, so a later play starts the next track.
   - "End of queue" is the natural stop, followed by the steps below.
3. **Put the factor back to 1.0**, only once the pause is confirmed: the
   pause's own 64-frame fade has passed and the stream is silent.
   Resuming then uses the existing fade-in from silence (StreamRestart,
   GainRamp, DeclickReader) at the listener's own level. Nothing resumes
   by itself.
4. **Screen off at once** (item 2's Off). The pocket guard applies to any
   wake.
5. **Release the headphones.** The A2DP stream suspends 3 s after the
   pause (StreamControl). After **5 min** paused, release them:
   `btSession.expectDrop()` so there is no "lost" dialog, disconnect,
   and go to the reconnect's Resting phase. This is a new path next to
   `letGoOfHeadphones()`, with three differences:
   - the output **stays Bluetooth**, so a later Play goes through
     PlayGate and pages them;
   - the device stays connectable;
   - no background paging.

   The headphones then turn themselves off. This saves nothing on the
   Core2 (an idle link costs ~0, measured), but it saves their battery
   and silences their keys.
6. **Idle power-off** (item 4) follows at its normal setting, counted
   from the pause. If it is set to Never, the device stays on in its
   quietest state (~22 USB mA).

### How it works with the rest

- **Volume.**
  - The fade is our gain only. **No AVRCP SET_ABSOLUTE_VOLUME is sent**
    for it, so the headphones' own level is untouched for tomorrow, and
    no late or rounded-up answer can land.
  - The listener's volume keys during the fade act normally, with the
    fade still applied on top.
  - The factor only falls by itself. It rises only on +10 min or
    Turn off, at the slow rate.
- **Skip during the countdown or the fade.** The timer keeps running and
  a fade keeps its level: the new track starts at the faded factor.
  - The "End of track" timer then applies to the new track. A skip is a
    deliberate act.
  - "End of album" re-reads the album of the new entry.
- **Pause during the countdown.** The timer keeps running, as on phones.
  - A pause during the fade finishes the timer at once: steps 3-6.
  - A timer that expires while paused skips the fade and runs steps 3-6.
  - A timer that expires during a PlayGate wait ends the wait paused
    (`cancelWait()`) and then runs steps 3-6.
- **Headphone keys after expiry.**
  - HeadsetKeys already never starts stopped music, but it does resume a
    pause, and in-ear detection sends Play too, for example when a
    sleeper turns over.
  - So a sleep-timer pause is marked "paused by the timer": **headphone
    Play doesn't resume it**, and the Core2's own play button does. The
    mark clears on any Core2 play.
  - Open question for the user: should a deliberate bud press resume
    anyway? The safer default is no.
- **Bluetooth drops during the countdown.** The existing rule pauses on a
  real disconnect, and the output stays Bluetooth. The timer stays armed
  and, at expiry, runs steps 3-6. **The timer never moves the output to
  the speaker.**
- **Output switch to the speaker during the countdown.** Switching to the
  speaker pauses first, as always. The timer and its factor carry over.

---

## 4. Implementation plan

Each step is a separate commit, keeps the host tests green
(`pio test -e native`), builds (`pio run -e core2` from PowerShell with
MSYSTEM unset), and ends with its on-device measurement. It uses the
probe (`Pm` markers, 60 s per condition, alternating A/B/A) and, from
step 9 on, battery-only runs (`Pw` plus `Pq1`).

### Step 0: land the probe, and fix what the measuring run found

**Status: done in the code, host-tested (test_power_window); the device
check is still to do.**

- `Pl`: the window a console command starts was timed from the command's
  `millis()`, a moment after the loop's `now`: `nowMs - start` wrapped.
  `power::Window::elapsedMs()`/`over()` now read a start in the future as
  "not begun".
- `Pt` with a link up says "applies from the next connection" with the
  range asked for, instead of the controller's stale read-back.
- `link=` comes from the reconnect planner's phase (step 1): `Pr0` shows
  `link=resting bg=resting`. The line adds `radio=N%`.
- `Ps0`'s swallowed wake touch logs `[screen] wake by touch at x,y (raw)`.
- ACIN glitches: a single sample of ≤15 mA on USB right after one above
  30 mA is held until the next; when that one is back above 30 mA it was a
  glitch, left out of the mean, min and max and counted (`glitches=N` on
  the line; not in the CSV, whose columns stay as they were). A drop that
  lasts counts.

- **Commit the power probe** as built: PowerProbe, PowerLab, PowerWindow,
  test_power_window, `tone:silence`.
- **Fix `Pl`:** its first window prints "4294967.5 s: no samples", an
  unsigned underflow of the window start at log start.
- **Fix `Pt`:** its confirm line reads back the old range while a link is
  up. Say "applies from the next connection" instead.
- **Fix `link=`:** it stayed "scanning" after `Pr0`.
- **Log each swallowed wake touch** with its position.
- **Filter ACIN glitches:** single-sample drops to 0-15 mA at 160/80 MHz.
  Keep them out of min/max, or flag them.

Host test: a test_power_window case for the first window.

**Device check:** `Pl` starts clean; `Pt` then a reconnect shows the new range.

### Step 1: reconnect back-off, resting, scan deadlines (item 1)

**Status: done in the code, host-tested (test_reconnect, test_ui_output,
test_ui_library, test_ui_nav; test_play_gate unchanged and green); the
on-device checks below are still to do.** As built:

- `ReconnectPlanner` (lib/core): phases Idle, Burst, Backoff, Resting and
  Scan; `start(why)` on a drop, at boot and on an ask; `pageMade()` from
  every `connect_to()`, `pageEnded()` from the DISCONNECTED that answers a
  page; `step()` on BtAppT's 250 ms ticks and heartbeats. A burst page
  goes at once, the next once the last has its answer and 10 s after it
  began (~25 s for the three). A page on its way when a burst starts is
  its try 1 (the boot's page included).
- The library's auto-reconnect is disarmed except during a pairing (whose
  tries are still the library's); `rememberPeer()` replaces the old
  re-arm on every link.
- The asks that start a burst: `BtSink::connect()` (PlayGate's Play, the
  card's Connect and tap, a B hold, console `o`), the Pair screen closing
  without a pairing, and `Pr1`. **Opening the Output tab is not an ask**
  (a deviation from item 1): the resting card would never be seen, and
  its Connect is one tap away.
- The quiet hook: `BtSink::setQuiet(bool)`, fed each loop pass with
  "screen off and nothing playing or waiting". Since step 2 the screen
  policy's Off feeds it (the probe's `Ps0` is that Off now). Fixed in
  review: not while the headphones are lost (`btLost`, a drop while they
  were the output): the drop pauses the player itself, so it counted as
  "nobody around" and the back-off never ran for a range drop. The
  resting card for lost headphones (only after the whole back-off now)
  reads "Back in range? / Tap Connect.", and the lost dialog "Stopped
  looking for them: Play tries again", instead of "They'll reconnect
  when switched on" (they may be on, their own reconnect given up).
- Scan deadlines: the scan by name (none remembered) runs 2 min after the
  boot or an ask, then rests. The Pair page stops its scan after 2 min
  (`PairSearch`) and shows "Search again" (a tap starts another 2 min).
  Since step 2 it also stops when the screen goes off. Fixed in review:
  those stops (`BtSink::pausePairScan()`) keep the background search held
  off while the page is up; they used to start the old headphones' burst
  and back-off with the page still open, as closing it does
  (`stopPairScan()`). The 2 min are checked every UI pass
  (`Page::tick()`): a dialog over the page stopped its `update()` and so
  kept the inquiry running.
- Radio busy: `RadioMeter` samples "a page on its way, or an inquiry" on
  every tick; `[stats] bt` shows `search=<phase> radio=N%/min`, and the P
  line `bg=<phase> radio=N%`.
- The card: `BtLink::Phase::Backoff` shows as "Looking for SPYDRONE..."
  (or Lost: "looking for"); `Resting` is its own card, "Not connected"
  with "They'll reconnect / when switched on." beside [Connect], dim (red
  when they dropped while the output), no spinner. The tab icon is the
  plain headphones while resting (`tabbar::Output::BtIdle`). A connect's
  burst that ends in Backoff or Resting is Failed (BtSession). Console
  `uiFr` fakes the resting card for a screenshot.
- Fixed with it: console `o` goes through the B hold's `selectOutput()`,
  so after a Disconnect it connects (and the audio moves once linked), and
  a move to the speaker pauses first.

**Code:**

- `ReconnectPlanner` gets the phases Burst, Backoff (30 s, 1, 2, 5 min,
  then 5 min) and Resting, plus a scan deadline. There is no background
  inquiry while remembered, and a listener ask restarts the burst.
- `BtSink` glue: disarm between pages and re-arm with 1 retry, as
  described in item 1.
- A per-minute "radio busy" counter in `[stats] bt`.
- An `OutputModel` resting line, and the tab icon not amber while resting.

**Host tests** (test_reconnect, with the library model extended):

- the back-off schedule;
- no discovery when remembered;
- the burst restarts on each kind of ask;
- a page in flight still counts as try 1 of a Play's burst;
- the scan deadline with none remembered;
- resting at once when the screen is off and nothing plays.

test_play_gate stays green. test_ui_output checks the new card text and
that it fits.

**On the device:**

- Headphones in the case, `Pl` for 20 min: after the burst the current
  falls to ~50 USB mA (±1), against 85.7 today. Radio-busy falls from
  ~85-100% to ~1%.
- Taking them out reconnects within ~5 s.
- Play pages at once ("try 1 of 3").
- The Pair screen stops at 2 min with "Search again".
- Rerun the ARCHITECTURE.md reconnect checks, with the "switch them off
  for 2 minutes" case adjusted to the new schedule.

### Step 2: screen policy and the swallowed wake (item 2)

**Status: done in the code, host-tested (test_screen_power, the latch
cases in test_ui_input, the settings texts in test_ui_library); the
on-device checks below are still to do.** As built:

- `ScreenPower` (lib/core): Bright, Dim (backlight 30) for the last 10 s
  (from 7 s with 15 s), Off after the chosen time without input: 15 s /
  **30 s** / 1 / 2 / 5 min / Never. Brightness Low 60 / **Medium 100** /
  High 160 / Max 255. Input is a touch that acts, a finger resting on the
  glass or the strip, or the PWR key while bright. `wake(why)`: Touch and
  PowerKey (from Off: the pocket guard, off again 10 s later with no dim
  step unless input follows; not with Never), Event (the whole countdown,
  and it ends a guard), Console. Kept Bright (and woken) while a screen of
  its own owns the display, a play waits for the headphones, or a pairing
  is under way (`link.phase == Pairing` or `btSession.pairingUnderWay()`).
  Fixed in review: it read `btSession.pairing()`, which stays set after a
  pairing fails (the Failed card says so), so a failed pairing kept the
  screen Bright for good, and the latch never armed.
- The countdown's input from a finger (`FingerActivity`, fixed in
  review): its landing and its moves (more than 8 px); a finger resting
  still stops counting after 15 s, so a pocket's pressure can't keep the
  screen lit. As it dims under such a finger, the latch takes it (its
  glass touch ends with a Cancel) without a wake.
- The pocket rule (fixed in review, item 2 above): `ScreenPower::
  unattended()` and `touchActs()`, `attend()` on a glass touch's landing
  (`Input::glassLanded()`) or PWR while lit; main's
  `ButtonTransport::startRefused()`.
- `WakeLatch` (lib/core), owned by `ui/Input`: a real finger landing on
  the panel while a touch doesn't act arms it; every event is dropped
  (the `suspended_` drop path) until no finger has been on for 400 ms
  (`kQuietMs`), so the lift (where a button clicks, a tap and a fling are
  made) is swallowed, and so is a second finger. Fixed in review: it let
  go after 2 quiet passes, ~10 ms at the lit loop's 1-5 ms, so the panel
  losing the waking finger for 20-140 ms (as StripButtons records it
  does) released it, and the finger found again was a fresh press: a
  click of B played out loud. 400 ms is StripButtons' swipe bounce
  window; a pass after a longer gap is a new touch however the passes
  ran. The recognisers keep following. The scripted
  finger isn't a finger for it: it acts in the dark, and wakes the screen
  (Why::Console) so what it does shows.
- `app/ScreenControl`: the hardware glue, the only caller of
  `setBrightness`, `sleep` and `wakeup` (the last two under `LcdLock`).
  Each pass: `beginPass()` (the PWR key's short press via `M5.BtnPWR`,
  USB in or out from AXP192 reg 0x00 once a second, `Input::setLit()`),
  `afterInput()` (the wake, logged `[screen] wake by touch at x,y (raw)`,
  with the strip button if it was one; else activity), and `step()` last
  in the loop. Off: the UI goes dark first, then backlight off and
  sleep-in. Wake: sleep-out with the backlight still off, 5 ms, the UI
  draws everything, then the backlight (`[screen] awake in N ms`). Fixed
  on the device: the UI used to draw while the panel was still asleep,
  and on this Core2 those pixels land garbled in the panel's memory
  (every wake: rows shifted sideways, colours byte-swapped, until the
  element was drawn again lit). A sleep-in or sleep-out waits until 120 ms after the last one;
  a touch meanwhile is still a swallowed wake. After a sleep-out, 5 ms
  before anything is drawn (the ILI9342C's wait before the next command;
  fixed in review: the next frame could come 1 ms later). Every change logs
  `[screen] <from> -> <to> (<why>)` with when the next one is due.
- The event wakes: `Ui::headphonesLost()` (the dialog), `Ui::playFailed()`
  ("Couldn't reach"), `Ui::noteFailures()` (a track that failed), and USB
  in or out, through `UiHost::wakeScreen()`. There is no low-battery
  warning yet, so none for it.
- The UI's dark mode (`Ui::setDark`): `gfx::fill`/`push` drop everything
  (also `ListView::pushItemInPlace` and `DanceView::pushRect`, which draw
  directly); `Ui::loop` keeps the snapshot, dialogs, toasts and their
  timers but skips the tab bar, page updates and frames; a fling stops
  where it is; `Thumbs::loop(now, busy)` takes in what the worker made
  and starts no new job; DanceMode is `setActive(false)` (its tracker and
  tap reader stop; since step 3 the outputs' taps too);
  `idleMs()` is 20. The page gets `screenOff()`: the Pair screen stops
  its scan and shows "Search again". On the wake: the list's scroll
  registers are sent again (`ListScroller::resend()`, the panel should
  keep them through sleep-in), the tab bar, the page (a list's band at
  its next frame), then the jump grid or coach cards, a sheet, the volume
  sheet, a dialog and the toast; a HUD from the dark is dropped.
  A screen of its own (calibration, spike tools) is never gated: the UI
  lets go of the gate when it is suspended.
- The quiet hook: `setQuiet(screen off and nothing playing or waiting)`.
- The console: `Ps` the state, `Ps0` the policy's Off (a touch's wake has
  the pocket guard), `Ps1` on; `Pb<n>` is the Bright level until restart
  (`Pb0` the setting's), refused while dim or off.
- Settings: the Output tab's rows "Screen off after" and "Brightness"
  (after Haptics), each a value pill; a tap takes the next choice (a
  sheet only has room for 3 rows). NVS namespace `screen`, keys
  `off_after` and `bright` (the choices' indices).

**Code:**

- `lib/core/ScreenPower`, fed with input times, player state, dialogs
  and power events.
- A wake-swallow latch in `ui/Input`, reusing the `suspended_` drop path.
- A dark mode in `Ui::loop`: no drawing, dance inactive, no thumbnail
  jobs, `idleMs` 20.
- Output-tab rows for "Screen off after" and "Brightness", saved in NVS.

**Host tests** (test_screen_power: timings, exceptions, wakes, the pocket
guard; test_ui_input: the latch):

- the latch with ButtonGesture and TouchRecognizer sequences;
- a B click, a B hold, a C hold and a swipe up from the strip in the
  dark each produce no event;
- the lift is swallowed too;
- a second finger;
- the scripted finger bypasses the latch.

**On the device:**

- `Pl` Bright vs Dim vs Off while streaming and idle. Expect -12 and -15
  USB mA, plus 1-2 from not drawing (estimate).
- Wake latency under 150 ms.
- Speaker as output, paused, screen off: tap B, hold B, hold C, swipe up,
  tap a row. The log shows only `[screen] wake`, and no audio starts.
- The same, then B again within 10 s: the inert buzz, "Tap the screen
  first, then B plays", no audio; a tap on the glass, then B plays.
- A long press of B from the dark, lifting and pressing again quickly
  (under 400 ms): nothing but the wake. A tap meant to act after a wake
  waits 400 ms from the lift.
- The lost dialog from a dark screen: the first tap only answers it
  (`[screen] event's wake answered by touch`), the second acts.
- A finger resting still on the glass: the screen dims 35 s after it
  landed (15 s + 20 s) and goes off; no wake while it stays.
- A failed pairing (headphones not in pairing mode), then leave it: the
  screen dims and goes off as usual.
- A wake mid-list leaves no corrupted band.
- Count false wakes over an hour in a pocket.

### Step 3: amp, IMU, EXTEN, taps (items 5 and 9)

**Status: done in the code, host-tested (test_output_chain's AmpGate
cases, test_audio_tap); the on-device checks below are still to do.** As
built:

- `AmpGate` (lib/core), asked by the speaker pump (the only task that
  queues buffers, so it alone calls `M5.Speaker.end()`/`begin()`): off
  (`end()`: the NS4168's enable, AXP192 GPIO2, first, then M5.Speaker's
  task and the I2S) 2 s after the pump last queued a buffer, once every
  queued buffer is back from M5.Speaker. "Quiet" is any pass with nothing
  to queue: paused, stopped with the speaker the output (the pump used to
  count that as playing and never went idle), the queue's end, an
  underrun of 2 s, or the output moved to Bluetooth. A pause under 2 s
  never switches it.
- On again before the next buffer: `begin()` (the I2S set up and
  clocking zeros), then 5 ms later the amp's enable, then 20 ms of zeros
  into the amp (`kAmpSettleMs`, so its start-up can't cut into the
  fade-in), then the audio, which the DeclickReader fades in as it always
  does after a gap. Fixed in review: M5Unified's enable callback raised
  the enable at the start of `begin()`, before the port was uninstalled,
  reinstalled and its pins reconfigured, now on every resume after 2 s;
  the pump replaces that callback (`M5.Speaker.setCallback`: nothing on
  begin, the enable down first on end) and raises the enable itself on
  the running clock (AXP192 GPIO2, or ALDO3 on an AXP2101 Core2). A fade-only buffer (the tail of audio that was never
  heard) doesn't switch it on.
- The log: `[speaker] amp off: I2S stopped, AXP192 GPIO2 low (quiet for
  2 s)` and `[speaker] amp on (audio to play)`, from the loop task.
- The console's `Pa`: `Pa0` switches it off as soon as the speaker is
  quiet (no 2 s wait); `Pa1` switches it on (zeros, silent) and holds it on
  through playing and pausing until `Pa0` (the P line's `amp=held`).
- The IMU: app/BoardPower suspends the BMI270 in `setup()` after
  `M5.begin()` (the code `Pi0` used, moved there; `Pi1` wakes it). One line
  says what boot left: `[power] boot: IMU suspended, 5 V boost (EXTEN) off,
  green LED off`.
- EXTEN: `cfg.output_power = false`. On USB the 5 V bus comes from USB
  anyway, so the measurement that the amp isn't fed from the boost was on
  USB: **the battery ear check below decides it.** If the speaker is
  silent on battery, the fallback is to switch EXTEN with the amp (on
  before `begin()`, off after `end()`), not to leave it on.
- Taps: `AudioTap::setEnabled()` (any task); a write while off copies
  nothing and the count stands still, and the first write after it is on
  again starts a new segment (the tracker never splices audio across the
  time it didn't see). Both taps are off from boot; `DanceMode` switches
  them with `active && tracking` (the Dance tab, the screen going dark,
  `Pk`). `AudioShared::tapOn` is gone; `Pk0`/`Pk1` now switch the tracker
  and the taps follow.
- The 20 ms loop delay while the screen is off was already step 2's
  (`Ui::idleMs`); main now also uses 20 ms when there is no UI (no PSRAM).

**Code:**

- Amp and I2S off 2 s after the speaker goes quiet.
- BMI270 suspended at boot.
- `cfg.output_power = false`.
- Taps written only while the Dance tab is up.

**Host tests:** test_audio_tap (a tap switched back on starts a new
segment); test_output_chain for the amp gating logic, if it is moved
into lib/core. As built: test_audio_tap's
`test_tap_switched_off_skips_and_starts_a_new_segment`; test_output_chain's
six AmpGate cases (off until audio; off 2 s after the last buffer and not
a pass sooner, on before the next, a short pause never switches; a
buffer still queued holds it; `Pa0` without the wait; `Pa1` held through
play and pause until `Pa0`; the clock wrap).

**On the device:**

- The boot line: `[power] boot: IMU suspended, 5 V boost (EXTEN) off,
  green LED off`; the P line `imu=suspended exten=off taps=off`.
- `[speaker] amp off ... (quiet for 2 s)` 2 s after a pause, a stop, and
  a move to Bluetooth; `[speaker] amp on (audio to play)` at the resume.
- After 10 s of speaker play, then pause, the current drops ~5 USB mA.
- **By ear:** no pop at the amp off and on handover, at 10% and 100%
  volume, with the headphones off your head.
- **By ear:** a resume on the speaker after >2 s paused starts cleanly
  (the 20 ms settle): the first beat isn't clipped. Tune `kAmpSettleMs` if
  it is.
- **On battery with USB unplugged:** the speaker is audible with EXTEN off.
- The Dance tab still locks within its usual time, also after leaving it
  and coming back, and after a screen-off on it (`taps=on` only while it
  is up).

### Device run: steps 0-3 (2026-09-27)

The working tree of steps 0-3 (plus the wake fix below), flashed on the
Core2 (COM3). Same method as section 1: on USB, the battery full (0.0 mA),
`in=` in USB mA, the mean of 12-13 five-second windows (60-65 s) after a
`Pm` marker unless said otherwise, 240 MHz (the default). Differences
under ~2.5 mA are noise. **The Powerbeats never answered a page during the
run** (7 bursts over ~40 min, the Pair screen's scan didn't see them
either): they were in their case or switched off. So everything that needs
a link is still to do (below).

**Measured:**

| State | USB mA | Against | Change |
|---|---|---|---|
| Headphones gone, screen on (bl 127), back-off (5 min: pages at +30 s, 1, 2 min) | 51.1 (60 windows, sd 8.5) | the old cycle, 85.7 | **-34.6** |
| Headphones gone, screen on (bl 127), resting (`Pr0`) | 47.7 | the old cycle, 85.7; the old "paused", 50.2 | **-38.0**; -2.5 (the step 3 knobs) |
| Headphones gone, a burst (3 pages, bl 127) | 73.4 (5 windows, sd 20) | the old paging windows, 60-84 | as before, now 25 s long |
| Headphones gone, screen off (the night case), resting | 29.2 | the old cycle with the screen on, 85.7 | **-56.5** |
| Idle, resting: bright at the new default (100) | 44.0 (43.9, 44.2: A/B/A) | | |
| Idle, resting: dim (bl 30; `Pb30`) | 35.7 (35.0, 36.3) | bright 100 | **-8.3** |
| Idle, resting: off | 29.2 | bright 100 | **-14.8** |
| Speaker playing (silent mode, FLAC, bl 100) | 65.5 | | |
| Speaker paused, amp off by itself | 45.5 (45.6, 45.3: A/B/A) | amp held on (`Pa1`), 50.7 | **-5.2** |
| The Pair screen scanning (bl 100) | ~89 (windows 2-24; 84.0 with the first) | after its 2 min stop, 44.5 | -44.5 while it would have kept scanning |

The idle "off" figure at 240 MHz (29.2) is below section 1's "screen off,
160 MHz" 28.5 + 4.7 (240 vs 160) = 33.2: the step 3 knobs (IMU, EXTEN,
taps) and the 20 ms loop delay, about -4, within noise of the -2.5 to -3.9
measured for them in section 1.

**Checked on the device (log):**

- Step 0: `Pl`'s first window is clean (no "4294967"); `Pr0` gives
  `link=resting bg=resting`; the line carries `radio=N%` (24% in a burst's
  windows, 100% while the Pair screen scans, 0% resting).
- Step 1 schedule: a burst's pages 10.1 s apart (346.9, 357.0, 367.1 s),
  "backing off" 5.4 s after the third, then pages 30.3 s after the third,
  then 60.2 s and 120.2 s apart, "the next in 300 s". With the screen off
  and nothing playing it rests right after the burst (boot, `Pr1`, the Pair
  screen closing). The resting card reads "Not connected / They'll
  reconnect when switched on." beside Connect (screenshot). `radio=` is
  the last *whole* minute's share, so it lags up to 60 s (26% for the
  minute after a boot's burst, then 0).
- The Pair screen: the scan stops at exactly 2:00 ("the search stopped
  after 2 min (tap Search again)", the row "Search again / Stopped, to save
  the battery.") and nothing pages while the page stays up (`bg=idle`, 44.5
  mA). "Search again" scans again; the screen going off stops it ("the
  screen went off"); it keeps scanning while dim. Leaving the page starts a
  burst.
- Step 2: off after 30 s (dim at 20 s, `bl=30 screen=dim`, then off);
  "Never" and the brightness choices are saved and come back after a
  reboot; the defaults (30 s, Medium 100) restored at the end. Console
  wakes (`Ps1`, `ui<n>`): `[screen] awake in 24-63 ms` on Now Playing and
  Output, 105-108 ms on the Dance tab (the redraw included).
- **Found and fixed: every wake left the screen garbled.** Screenshots
  (LCD readback) after `Ps0`/`Ps1` showed the elements drawn at the wake
  shifted sideways within their rows with byte-swapped colours (the play
  button green, text magenta), identical in two shots of the same wake, on
  Now Playing and the Output list; one drawn again while lit was clean, and
  so was every screen before the first screen-off. The pixels were written
  while the panel was in sleep-in. `ScreenControl::apply()` now sends the
  sleep-out first (the backlight still off), waits the 5 ms, lets the UI
  draw, then turns the backlight on: 4 of 4 wakes clean since (Now Playing
  three times, the Output list scrolled mid-way), 5 of 5 garbled before.
- Step 3: the boot line `[power] boot: IMU suspended, 5 V boost (EXTEN)
  off, green LED off`; the P line `amp=off exten=off led=0
  imu=suspended taps=off`. `[speaker] amp off ... (quiet for 2 s)` 2.01 s
  after a pause (twice); a 1.1 s pause didn't switch it; `amp on (audio to
  play)` 44-79 ms after the play key; `Pa1` shows `amp=held`, `Pa0`
  switches it off at once. `taps=on` only while the Dance tab is shown
  (off on Now Playing and while the screen is off, on again after). The
  tracker locked on "Stronger" 6.2 s after the reset (3.3 s on another
  run), 9.8 s after leaving the tab and coming back, and 14.4 s after a
  screen-off on the tab (it loses and finds this track's beat now and then
  either way).
- The probe: at 240 MHz the glitch filter fires too (up to 85 in 5 min at
  ~50 mA), and some windows still show `min 0.0` with no glitch counted:
  drops of two or more samples to 0 mA, which the filter keeps by design.
  The means are unaffected at the noise level above.

**Still to do (needs the headphones, a finger, or an ear):**

- Headphones: taking them out of the case reconnects within ~5 s while
  resting; Play pages at once ("try 1 of 3"); `Pt` then a reconnect shows
  the new range; streaming `tone:silence` with the screen bright (100), dim
  and off, and the streaming baseline at the new default (expected ~106:
  109.8 at 127 minus 3.1, minus the knobs); the headphones-lost dialog and
  "Couldn't reach" waking a dark screen, and its first tap only answering it.
- By hand (the scripted finger bypasses the wake latch by design, so it
  can't test it): from the dark on the speaker, tap B, hold B, hold C,
  swipe up, tap a row: only `[screen] wake by touch`, no audio; B again
  within 10 s: the inert buzz and "Tap the screen first, then B plays"; the
  pocket guard (a touch or PWR wake from off, nothing more: off again 10 s
  later); a finger resting still: dims and goes off; real-touch wake
  latency; the screen really looks right after a wake (the readback says
  so now).
- By ear: no pop at the amp's off and on at 10% and 100%; a resume after
  more than 2 s paused isn't clipped; on battery with USB unplugged, the
  speaker is audible with EXTEN off.
- USB in or out waking the screen; false wakes over an hour in a pocket.

### Step 4: sleep timer (section 3)

**Code:**

- `lib/core/SleepTimer`: the state machine, the fade curve and the order
  of release.
- A fade stage in both outputs.
- `PlaybackController`: "pause after this track" and the "paused by the
  timer" mark, which HeadsetKeys reads.
- A new `releaseHeadphones()` next to `letGoOfHeadphones()`.
- The UI: the "..." row, the sheet, the indicator and badge, and the fade
  toast.
- A console command `T<min>`, `Ts<sec>` for tests, `Tt`, `Ta`, `Tq` and
  `T0`. Uppercase T is free.

**Host tests** (test_sleep_timer, test_playback, test_headset_keys,
test_output_chain, test_ui_*):

- expiry for each choice;
- the fade factor never rises by itself and never exceeds 1.0;
- a skip or a pause during the fade;
- expiry during a wait;
- +10 min and Turn off ramp up slowly;
- the release order: pause confirmed, then factor back to 1, then screen
  off, then release at 5 min;
- headphone Play is ignored after a timer pause; the Core2's play works;
- the output never changes.

**On the device:**

- `Ts60` on the headphones. The log shows fade start, pause, factor
  restored while silent, screen off, stream suspended 3 s later, and the
  headphones released at 5 min with no "lost" dialog.
- Listen for a smooth fade with no step or click.
- `[stats] bt` shows no AVRCP volume command during the fade.
- The same on the speaker, in silent mode `z`.
- `Pl` through it: ~110, then ~50, then ~33 (screen off), then idle.
- Afterwards, Play pages and fades in.

### Step 5: idle power-off (item 4)

**Code:** `lib/core/IdlePolicy`, `QueueStore::flushNow()`, the NVS flag
and boot toast, and the setting row.

**Host tests:** test_idle_policy covers:

- every condition that blocks it: playing, waiting, USB, Pair screen,
  pending write, input;
- the countdown restarting on input.

A queue test covers `flushNow()` in the middle of a piece-wise write.

**On the device:**

- A 2 min timeout from the console. The log shows `[power] off after
  idle`, the device is dark and silent, PWR boots it, and the queue and
  position come back, stopped.
- With USB in, it does not power off (and check what ACIN does to an
  off device).
- A power-off right after a queue edit loses nothing.

### Step 6: CPU 160 by default, then the 80 MHz governor (item 6)

**Step 6a: 160 by default.** Before changing the default, run at `Pcb160`:

- the 60-min Bluetooth soak (0 underruns);
- `w1`-`w3` scroll stress (fps, frame max, ring min);
- `b<n>` on MP3 and FLAC;
- `[dance]` fps with an MP3;
- `[audio] refill` times at a track start.

If they hold, make 160 the default. **Device check:** `Pl` shows -5 USB mA
streaming.

**Step 6b: the idle governor.** A CpuGovernor that switches 160 and 80.
Host tests cover when it may drop. **On the device:**

- linked and paused, toggle every 5 s for 30 min: 0 disconnects, and
  AVRCP keys still work;
- speaker pitch is right after a switch;
- `Pl` shows -4 USB mA idle.

### Step 7: TX floor (item 7)

**Code:** `esp_bredr_tx_power_set(N12, P3)` at stack start.

**On the device:**

- 30 min streaming with the Core2 in a pocket and at 5 m: underruns and
  `gap=` unchanged;
- `Pl` A/B against the default, each after a fresh connection.

### Step 8: Dance frame rate (item 8)

**Code:** 10 fps idle, 24-30 fps dancing.

**Device check:** `[dance]` fps and beat error on the click tracks
unchanged while dancing; `Pl` on the Dance tab paused, 30 vs 10 fps
(expect ~-3 USB mA).

### Step 9: battery-only validation

**First, a rundown test.** `Pw` and `Pq1`, unplug, and stream a fixed BT
playlist at a fixed volume, with the screen timing out, until the
AXP192's cut-off. Compare against a rundown at step 0's firmware. This
replaces the bat-mA approximation.

**Then two overnight runs**, each with the headphones gone:

- one with a 30 min sleep timer;
- one idle with the screen off.

Check the battery % in the morning against the estimates in section 1.

**Later:** DFS and PM locks, folded into the WiFi `custom_sdkconfig`
rebuild (item 6.3).

### Not energy, found while measuring (for the user to decide)

- **`p` while stopped starts playback** (console, and presumably the strip's
  A click), out loud on the speaker. The Core2's own buttons skip-and-play
  by design, but from Stopped on the speaker this plays unexpectedly. The
  screen-off swallow covers pocket presses; lit, it is a deliberate press.
  Confirm it is intended.
- **Console `o` after a Disconnect** selected Bluetooth but started no
  connection: the card showed "Not connected", and nothing paged until
  Connect was tapped. **Fixed in step 1:** `o` does what a B hold does.
- **The Powerbeats stopped answering pages and inquiry for ~40 min** in
  the measuring run, then connected at a later boot. It is probably the
  headphones, but step 1's on-device checks should note it if it recurs.
