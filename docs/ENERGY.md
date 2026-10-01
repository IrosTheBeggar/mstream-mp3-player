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

The "after the plan" column counts 160 MHz. The default stayed 240 (step
6a: list scrolling halves at 160 with an MP3 playing), so ~5 USB mA of it
(~0.1 h of listening) needs "CPU speed" set to 160 on the Output tab.

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
   nothing is remembered and there is a name to look for: a developer
   build's `BT_SINK_NAME` (or the console's one-scan `Bs`). A release
   build has none, so with nothing remembered it never scans by itself,
   at the boot or on any ask: it rests at once, connectable only, and
   pairing is the Pair screen's alone (`SinkSearch`).
   The Pair screen's scan stops after 2 min and
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
- **Held lit** (added after batch 3, `holdLit`): while a toast with a
  countdown is up (the idle power-off's warning, item 4; the sleep
  timer's fade while it counts down to the pause, section 3: not a
  track's fade held after a skip), a lit screen (bright or dim) goes bright and
  stays lit until it ends; an off one stays off (it may be night). A
  screen woken during it is held from the wake, except that a touch's or
  PWR's wake from Off keeps its pocket guard until input follows. For the
  idle warning that wake is input anyway, which ends the warning.
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

**Status: in the code (step 4), host-tested (test_sleep_timer, and new
cases in test_playback, test_headset_keys, test_output_chain, test_ui_nav,
test_ui_library); the on-device checks are still to do.** The as-built
notes are under step 4 in section 4.

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

**Status: in the code (step 5), host-tested (test_idle_policy, and the
`flushNow()` cases in test_queue); the on-device checks are still to do.**
What follows under "The change" is the plan, with one addition (a
warning, below); the as-built notes are under step 5 in
section 4.

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

It does not light the screen: it may be night. (As built, added since the
plan: for its last 30 s a toast "Turning off in 30 s" with **Keep on**
is up, seen on a lit screen, for example with Screen off after: Never; any
input keeps it on. Since batch 3 a screen lit when it appears stays lit,
brightened, until it ends, rather than dimming and going dark partway
through its countdown: item 2's "held lit".) At the next boot a toast says "Turned off after 20
minutes idle". PWR boots
the device, stopped, where it was, as today, and (since the resume point,
"The resume point" in section 4) at the second it paused at: play picks
up there.

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
  Item 8 now aims for a steady 24 at 160 (and 10 while the dancer idles).
- **List scrolling halves** (step 6a, measured): the scroll lab's stress
  with an MP3 over Bluetooth runs at 7.8 fps against 15.9 at 240, a FLAC
  at 15.5 against 24.7. **So the default stays 240**; 160 is the CPU speed
  row's other choice.
- **Tracks at other rates** (since the rate converter, RESAMPLER.md): a
  44.1 kHz track's passthrough is the old path again, 5 cycles a frame
  cheaper, but against the build before the converter an MP3 measured
  0.8 points of a core more at 240 MHz (the MP3 decoder's loop runs 2 %
  slower in the new image, the cache) and a FLAC 0.3-0.4 more
  (RESAMPLER.md section 10b); a 48 kHz one adds the conversion on the
  decode task, 5.8-5.9 % of a core at 240 MHz and 8.8 % at 160 since the
  MAC16 kernel (`Rb`, RESAMPLER.md sections 10 and 10b; with the C
  kernel, the fallback if the boot self-test fails, about 16 % and 25 %;
  22 % and 32 % were 771f8ae's per-frame path). With a 48 kHz tone list
  scrolling runs at 26.6-26.9 fps at 160 (29.0-29.6 with 44.1 kHz; 9
  before). A 48 kHz MP3 (a proxy) at 160 played 14.1 minutes without an
  underrun, but its start refills about twice as slowly as a 44.1 kHz
  MP3's and stalls the UI 2.5 times as long, and while it plays the
  scroll lab runs at about 11 fps against 27: its decoding alone takes
  about 51 % of the core there, the whole track about 64 %.
  88.2/96 kHz files are refused at 160 MHz (an estimated 63-79 % of core 1
  for a 24/96 FLAC there), provisionally until measured; for now they are
  off at any speed until the device check (RESAMPLER.md, section 6).

**The change:**

1. **Fixed 160 from boot** (what `Pcb160` does, as the default; tried,
   and back to 240 after step 6a: see **Default** below), before
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

**Default: 240** (`kDefaultCpuMhz` in lib/core/PowerChoices, one
constant). Batch 3 shipped 160 as the default, pending the step 6a soak;
the soak had 0 underruns and the bench held, but list scrolling with an
MP3 playing ran at half the frame rate (outside the ~15% allowed), so the
constant went back to 240 ("Device run: batch 3", step 6a). 160 saves ~5
USB mA streaming for whoever picks it in the row.

**Status: 1 is in the code (batch 3), as a setting; host-tested
(test_power_choices, the texts in test_ui_library); checked on the device
("Device run: batch 3", after step 8: the row, the dialog, restarts both
ways on the headphones and on the speaker; two restart-wait bugs found and
fixed there). Step 6a ran: the default is back to 240.** The user asked for it in the UI, so it is a setting after all,
restart included:

- **"CPU speed"** on the Output tab, after "Turn off when idle": **240 MHz**
  "Smoothest lists, dancing" / **160 MHz** "Slower lists, saves a little"
  (167 of the line's 174 px), the value in a pill, a chip icon. Batch 3's
  "Saves battery, a bit slower" was too rosy: at 160 a list scrolls at
  about half the frame rate with an MP3 playing (~8 against ~16 fps), the
  Dance tab runs ~18 of its 24 fps, and the saving (-5.2 USB mA streaming
  in the audit) was lost in the noise of the batch 3 run.
- **A tap asks first**: the dialog "Restart at 160 MHz?", "Saves a little
  battery; lists scroll at half speed while music plays. Music pauses and
  picks up at the same second." (to 160: the cost as well; the title says
  it restarts), or "Restart at 240 MHz?", "The speed changes at a restart.
  The music pauses and picks up at the same second." (to 240); [Cancel]
  [Restart]. Both bodies fit the dialog's 3 lines (test_ui_library).
  Cancel saves nothing. (Batch 3 said "the queue and your place are
  kept", and only the entry was: see "The resume point" in section 4.)
- **Restart**: the choice saved; paused first; then the idle power-off's
  orderly way (`QueueStore::flushNow()`, a note for the next boot, the
  headphones let go: no "lost" dialog), and the speaker's amp switched off
  the orderly way (its enable before its I2S, once the pause's fade has
  played out, as `Pa0`: `esp_restart()` doesn't reset the AXP192, so a
  live amp would hear its clock pins reconfigured through the reset, a
  pop). Once both are done (at most 3 s, `powerchoice::cpuRestartDue()`;
  the link counts as gone at DISCONNECTED, `BtSink::linkUp()`, not when
  the library's `connected()` drops as the disconnect starts)
  `esp_restart()` with the panel's SPI lock held. A toast "Restarting at
  160 MHz..." stays up until then (5 s). The next boot shows "CPU
  speed: 160 MHz" for 6 s (the idle power-off's boot-toast path). After
  the restart nothing plays by itself: the queue comes back stopped, at
  the same entry, as after any boot, and at the same second: the pause
  before the flush saved the resume point, Now Playing shows it, and play
  picks up there (section 4, "The resume point"); the headphones reconnect
  as at any boot, and take the output as at any boot.
- **Storage**: NVS "power"/"cpu_mhz", shared with the console's `Pcb`: an
  explicit 160 or 240; absent, or anything else, is the default. `Pcb160`
  / `Pcb240` save it, `Pcb0` removes it (the default). `Pc` (runtime 160
  <-> 80) is unchanged.
- After a `Pcb` that differs from the clock, the row shows the saved
  choice and the line "240 MHz until a restart"; a tap then saves the
  running speed back, with no restart.
- **Not during a pairing** (the Pair screen's scan, or one picked there):
  the restart would drop it, maybe half-bonded (the idle power-off waits
  for one too). A tap then shows "Wait for the pairing to finish" and
  saves nothing; the host refuses it as well.

**2, the 80 MHz idle governor (step 6b), is deferred**: the idle power-off
(item 4) already caps idle time on battery, and 80 MHz would save ~4 mA
only in the idle minutes before it. It comes back if the battery runs
(step 9) show idle time that the power-off doesn't cover.

| | |
|---|---|
| **Setting** | "CPU speed": **240 MHz** "Smoothest lists, dancing" / 160 MHz "Slower lists, saves a little" (a restart, asked first; to 160 the dialog says lists scroll at half speed while music plays) |
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

**Status: in the code (batch 3), as a setting the user asked for;
host-tested (test_power_choices, the texts in test_ui_library); checked
on the device ("Device run: batch 3": every choice, the readback after a
fresh connection, A/B streaming; the range soak is still to do).** The
listener now picks the ceiling (every range
starts at -12 dBm, so the headphones' power control can always turn it
down):

| Choice | Levels | dBm | The row's line |
|---|---|---|---|
| Low | 0..2 | -12..-6 | "Saves battery; stay close" |
| **Normal** (the default) | 0..5 | -12..+3 | "Adjusts to the distance" |
| High | 0..7 | -12..+9 | "More range, more battery" |

(The spec's "keep the player close" and "uses more battery" don't fit
beside the pill.)

- **"Bluetooth power"** on the Output tab, after "CPU speed"; a tap takes
  the next choice (after High, Low), saved at once in NVS
  "power"/"bt_tx" (absent: Normal), 4 signal bars as its icon (2, 3 or 4
  lit).
- **Applied** at once (`BtSink::setTxPower()`), and at every stack start:
  `PlayerA2dp::bt_start()` calls the library's controller start, then
  sets the levels, before Bluedroid is enabled, so before any page, scan
  or page scan (the boot's first page is the stack-up event's, 10 s
  later; every later page is ReconnectPlanner's, after it). `[bt] tx
  power: -12..+3 dBm (levels 0..5), from the stack's start` at boot.
- **A link that is up keeps its level** (measured), so a change while
  linked applies from the next connection: the row's line reads "From
  the next connection" until the link goes (`BtLinkLevel`: the choice the
  link was made with). Nothing reconnects by itself.
- The console's `Pt` stays a test override until restart (not saved; a
  change on the row wins) and reports the setting too; `P` lines show
  `tx=-12..+3 dBm (Normal)`, or the `Pt` levels until the row is next
  tapped (then a line says the row replaced them, and `P` shows the row's
  range again).

| | |
|---|---|
| **Default** | Normal (-12..+3 dBm) |
| **Setting** | "Bluetooth power": Low / **Normal** / High |
| **Risk** | low with the ceiling unchanged; watch underruns and `gap=` at range (Low: at arm's length and in a pocket) |
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

**Status: in the code (batch 3); host-tested (test_dance_rate); checked on
the device ("Device run: batch 3"): the rates and switches as specified,
but with an MP3 playing the dancer doesn't reach its target (~17/24 at
160, ~25/30 at 240), and the idle rate's saving is below the noise at
160.** As built:

- **The policy** is `DanceRate` (lib/core). The mode comes from what the
  last frame showed: **idle** when no beat was heard (paused, stopped, a
  starved output: the tap reader has nothing audible) or the tracker isn't
  locked, *and* the dance weight is 0.5 or less; otherwise **dancing**
  (also the console's frozen pose, `k<n>`). So a fade-out after a pause
  runs at the dancing rate until the weight is under 0.5, and a locked
  beat is danced to, fade-in included, from the next idle frame (at most
  100 ms). It applies to both skins (the crab, the stick figure).
- **The rate:** idle 10 fps (100 ms); dancing 30 fps (33 ms) at 240 MHz,
  24 fps (42 ms) below it, from `getCpuFrequencyMhz()` at each frame, so
  the "CPU speed" row and the console's `Pc` both count.
- **No catch-up burst:** `dancerate::Pacer` keeps the frames on deadlines
  as before; a new period is counted from the last frame drawn, and its
  first frame starts the new cadence (the `lastFrameMs_` of the plan is
  gone).
- **Screen off:** already no frames: `Ui` turns `DanceMode` off when the
  screen goes dark (and on again at the wake, on the Dance tab), and
  `DanceMode::loop()` draws nothing while off.
- **Observable:** each change logs `[dance] 10 fps (idle)` or `[dance] 24
  fps (dancing at 160 MHz)` (also once when the tab comes up), and the 5 s
  `[dance]` line reads `fps=23.8/24 (dancing)`: measured over the last
  second / the target, and the mode. The console's `ui` prints `[ui] page:
  Dance: ... 24 fps (of 24)`.

| | |
|---|---|
| **Default** | on |
| **Setting** | no (it follows the "CPU speed" row's clock) |
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
- Scan deadlines: the scan by name (none remembered, a build with
  `BT_SINK_NAME`) runs 2 min after the boot or an ask, then rests. A
  release build doesn't scan at all (`sinksearch::mayScan()`; the
  library's own stack-up scan is held off too): `start()` with nothing
  remembered and no name goes straight to Resting. The Pair page stops its scan after 2 min
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
- no scan at all with none remembered and no name (a release build), at
  the boot and on every ask (added when pairing became the Pair screen's alone);
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
- Checked on the device (2026-09-30): with nothing remembered and no name
  (the console's `Bf` boot and `Bn` session, which leave the stored
  pairing alone), no scan at the boot or on any ask for 6 min: no
  `[stats] bt` line (search resting, radio 0 %). Details in
  ARCHITECTURE.md (Developer tests).

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
  screen Bright for good, and the latch never armed. Since batch 3 a lit
  screen is also held Bright (never woken) while a countdown toast is up:
  `step(now, keepLit, holdLit)`, "The 160 MHz texts and the held screen".
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

**Status: done in the code, host-tested (test_sleep_timer; the new cases
in test_playback, test_headset_keys, test_output_chain, test_ui_nav and
test_ui_library); checked on the device on the speaker (silent mode), see
"Device run: steps 4 and 5" below; everything that needs the headphones,
a finger or an ear is still to do.** As built:

- `SleepTimer` (lib/core): phases Off, Counting (a timed choice), Armed
  (End of track / album / queue), Fading, Ending (the pause asked for,
  not yet confirmed) and Ended (shown as off; the release still to come).
  `update(In)` every loop pass, before `PlaybackController::update()`
  (main.cpp's `stepSleep()`), returns what to do: the fade's target, "pause
  after this track", pause now, restore, screen off, release. The choices
  are RAM only; the console's `Ts<sec>` takes any length.
- The fade: a timed choice fades over the 30 s after it expires, linear in
  dB to -40 dB (`curveQ15()`), then 0 for `kZeroMs` (100 ms), then the
  pause. End of track / album / queue fade over the track's last 10 s
  when its length is known (the backend's `durationMs()`: read from the
  file, or the estimate, settled by then), down to -40 dB at the
  boundary; unknown length: no fade. The target is held: it only ever
  falls while the timer runs (a skip keeps it; after a skip during a
  track fade the level stays where it was until the new track's own last
  10 s). It rises only on +10 min, Turn off, another choice, a Core2 play
  before the pause settled, and `restore()` once the pause is confirmed.
- A skip's stale position: the queue's entry changes at once, but the
  backend starts the new track a moment later and reports the last one's
  position and length until then (near its end, End of track would fade
  the new track to about -40 dB at once, and hold it). `EntryStart`
  (lib/core, beside `SleepTimer`) says when they are the new entry's: the
  entry changed with the position under 1 s, or since then the backend's
  start count moved or the position went back. Until then `stepSleep()`
  hands the timer a length of 0 (unknown: no fade), and "what the track
  has left" (+10 min's) is unknown too.
- `FadeStage` (lib/core), in `AudioShared`: ONE stage for both outputs, an
  atomic Q15 target (clamped to 1.0) and an atomic Q30 level. Down at most
  full scale in 1024 frames (it smooths the loop's ~20 ms steps of the
  target, ~0.03 dB each); up at GainRamp's slow rate (~20 dB/s). Applied in
  `BtSink::onData` after `gain_.process` and in `SpeakerSink::pump` after
  the tap write, before `playRaw` (the beat tracker hears the music). Only
  the output that is the ring's consumer moves the level (two tasks moving
  it would double its rate); the other applies it as it is. The level is
  published with a compare-exchange, so a `restore()` in between wins.
  Bit-exact at 1.0. Never AVRCP: the headphones' own level is untouched.
- "Pause, never stop": `PlaybackController::setPauseAfterTrack()` (at the
  natural end: the next entry, paused at 0:00, cued; at the end of the
  queue without repeat, the natural stop; a skip or a failure isn't an
  end, the flag stays) and `pauseByTimer()` (playing pauses, a wait ends
  paused, a pause is marked; stopped: nothing). `timerStops()` counts the
  boundary pauses; the timer sees one the pass after it happens. End of
  queue is End of track on the queue's last entry (with repeat: paused on
  the first entry). End of album is the next entry on another album
  (`SleepTimer::albumEndsBetween()`: the album; the folder for the loose
  tracks; a built-in track is an album of its own; the queue's end ends it
  too).
- The mark: `pausedByTimer()`; `HeadsetKeys::decide(state, key,
  pausedByTimer)` ignores Play then (main.cpp logs `[bt] headphones: play
  (ignored: paused, paused by the sleep timer)`); their Next/Prev still
  only select, and keep the mark. Any play clears it (every path to
  Playing or Waiting goes through `setPlaying()`), as does a stop.
- The order at expiry: the pause; then, once nothing played for
  `kSettleMs` (250 ms, far past the pause's 64-frame fade), `restore()` and
  the screen off in the same pass (`ScreenControl::sleepTimerOff()`,
  `ScreenPower::Why::SleepTimer`: a touch's or PWR's wake has the pocket
  guard); 5 min after that, the release. A Core2 play before the pause
  settles ends it there (the fade comes back up slowly, nothing else
  happens); a play before the 5 min cancels the release.
- `releaseHeadphones()` (main.cpp) next to `letGoOfHeadphones()`: linked,
  `btSession.expectDrop()` (no "lost" dialog) and `BtSink::
  releaseHeadphones()`, a new ask (bit 256: `askPending_` is 16 bits now)
  carried out on BtAppT (`PlayerA2dp::userRelease()`): disconnect with
  `releasing_` set, and the drop (`linkDown()`) rests the planner instead
  of starting a burst; still connectable, not `userOff_` (so not refused).
  Unlinked: the search rests. The output is left as it is (Bluetooth: a
  later play waits and pages them through PlayGate). A connect() drops a
  release still queued.
- A play between the release and its drop (the Core2's play in that
  moment, or a touch that cancels the idle power-off's release): the link
  was still up, so nothing held the play and it runs on Bluetooth. When
  the drop comes, `BtSession::onDisconnected()` says pause it (no dialog:
  the drop was expected), else the player would show Playing with no link
  and the headphones, coming back later, would start the music in the
  listener's ears. A play after the drop waits and pages them, as ever.
  `[bt] let go while a play had just started: paused (play pages them)`.
- The UI:
  - Now Playing's "..." sheet has 4 rows now (Sleep timer first, its state
    dim on the right: `rowText()`, which follows the timer while the sheet
    is up: `Sheet::setDetail()`). Every sheet row stays 40 px
    (`lib/core/SheetLayout.h`): a 4-row sheet rises into the header row
    (from y 40, as the dialog does), so the panel sprite is 320 x 204 (the
    content area) instead of 320 x 168; up to 3 rows stay in the list's
    band. A toast up draws over its title row, and when the toast goes the
    sheet is drawn again.
  - The Sleep timer sheet (`ui/Overlays`' `SleepSheet`, the sheet panel
    from y 72): the title with the state ("Sleep timer: 23 min left",
    "Sleep timer: end of album", "Sleep timer: fading",
    `SleepTimer::titleText()`: lower case after the colon, as the toasts
    say it; only the "..." row's detail alone is capitalised) and the
    close pill; "15", "30", "45", "60", "90 min" (Body); "End of
    track", "End of album", "End of queue" (Small: "End of album" is 106 px
    in Body); while one runs "+10 min" and "Turn off" (red), else a line,
    "Fades out, pauses, then the screen goes off." The running choice is
    outlined in the Now Playing accent. It follows the timer while open.
  - The moon: drawn pixel by pixel (`icons::drawMoon()`), 11 px on Now
    Playing's progress line with `shortText()` ("23 min", "45 s" in the
    last minute, "track", "album", "queue", "fading", amber while fading),
    6 px after the line. When both don't fit, `uitext::sleepLineFit()`
    decides what gives way: first "· <output>" ("4 of 16 · SPYDRONE" is
    2 px too wide with "23 min", measured: "4 of 16" and the moon stay),
    then the line (the moon alone). The amber "SPYDRONE (not connected)"
    (a drop during the countdown) is never dropped for it: the moon and
    its text beside it, else the moon alone beside it, else only the
    warning (the tab bar's moon still shows the timer; "SPYDRONE (not
    connected)" is 177 px of the 190, measured: the warning alone). The
    tab bar's
    7 x 7 moon in the Now Playing cell's top-right corner (amber while
    fading); `tabbar::State::sleep` has no minutes, so the cell redraws
    only when it starts, fades or ends.
  - The fade's toast ("Sleep timer: fading" in Small, [+10 min] [Turn off]
    in Body, Turn off to the screen's edge): shown when the fade starts
    and again on any touch, strip press or PWR wake during it (a wake is
    swallowed as usual: `ScreenControl::takeWoken()`, the pass's Down or
    button event); it stays up until the fade ends. (Since batch 3 a screen
    lit when the fade starts stays lit through it, brightened, and one
    woken during it too, except that a wake from Off keeps its pocket
    guard until input follows: `ScreenPower`'s `holdLit`. The pause's
    screen off still turns it off. Only while the fade counts down to the
    pause, `SleepTimer::fadeCountingDown()`: a track's fade held after a
    skip lasts until the new track's last 10 s, or the album's end, and
    the screen times out as ever meanwhile.) A tap elsewhere on it
    only hides it. Its buttons take a tap to y 77 (42 px; the toast is
    drawn 36 px) while no sheet is up, and they come first even over an
    open sheet (the "..." sheet, the volume sheet, the Sleep timer sheet:
    the toast is drawn on top of them), so a tap on +10 min never only
    closes a sheet.
  - The pocket rule for the toast (`SleepTimer::toastTap()`): both buttons
    bring the music back up, so neither acts on a clamped reading at the
    right edge (fabric pressure makes those), nor on the touch that
    attended a screen woken from off (`ScreenPower::glassLanded()` /
    `landedUnattended()`): in a pocket the first contact wakes the screen
    (swallowed) and the next lands on the lit toast. That touch attends
    the screen (B plays on the speaker again) but the toast shows no press
    and does nothing; the listener's next tap acts. So after a wake from
    off it is two taps: one to wake, one on the glass, then the button.
  - Choices toast what they did ("Sleep timer: 30 min", "Sleep timer: end
    of album", "Sleep timer off", "Sleep timer: 33 min left" after +10).
- +10 min never leaves less time than before (`SleepTimer::canExtend()`):
  it adds to what is left (Counting); during a timed fade it is 10 min
  from now; End of track, and End of album / queue on the album's or
  queue's last track, become a timed one of what the track has left plus
  10 min. End of album / queue before that last track: what is left isn't
  known (more than this track), so +10 min is refused and drawn dim on
  the sheet (`AppState::sleepCanExtend`); the same for a track of unknown
  length. A track's fade after a skip away from the boundary track (End
  of album): +10 min brings the level back up and the timer waits for the
  album's end again. Another duration restarts from now.
- The console: `T` status (`[sleep] 23 min (counting, timed); fade: none;
  player playing`), `T<min>`, `Ts<sec>`, `Tt`, `Ta`, `Tq`, `T+`, `T0`.
  Every step logs a `[sleep]` line (fading, the pause, the fade restored,
  the screen off, the release; `[bt] release: letting go of ...` and
  `[bt] reconnect: resting (let go: the sleep timer, or turning off)` from
  BtSink, shared with the idle power-off since step 5).

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

As built: test_sleep_timer (28 cases with the real PlaybackController and
FadeStage, carried out as `stepSleep()` does: each choice's expiry,
unknown lengths, End of album and End of queue with and without repeat, a
skip and a pause during the fade, a random property run that the target
never rises by itself, +10 min and Turn off at the slow rate, the release
order and its timings, a play before the pause settles or before the
release, headphone Play, a drop during the countdown, +10 min never
shortening an end-of choice and re-arming after a skip, `EntryStart` and
End of track after a skip near the end (with a backend that keeps
reporting the old track: no fade; without the guard, the fade the bug
was), the fade toast's buttons (`toastTap()`: clamped readings and the
attending touch act on neither), the texts and titles, the curve,
`albumEndsBetween()`); test_playback (pause after this track, its
survival of a skip and a failure, the queue's end, the mark);
test_headset_keys (Play ignored after a timer pause; which keys are idle
input); test_output_chain (the fade in the chain's never-adds-level run;
bit-exact at 1.0; falls smoothly, rises at ~20 dB/s; only the consumer
moves it; `restore()`); test_screen_power (the touch that attends is
remembered); test_ui_output (`BtSession::onDisconnected()`: a play between
a release and its drop is paused); test_ui_nav (the moon's cell and room;
no sheet row under 40 px); test_ui_library (every sleep timer text in its
room, the titles lower case, what the progress line drops for the moon).

Not host-tested: the glue in main.cpp (`releaseHeadphones()`,
`stepIdle()`'s order of flush, note, release, wait and power-off, the last
USB read). The decisions it carries out are the portable classes' (each
tested above); the order itself is one function, read in review and
checked on the device (below).

**On the device:**

- `Ts60` on the headphones. The log shows fade start, pause, factor
  restored while silent, screen off, stream suspended 3 s later, and the
  headphones released at 5 min with no "lost" dialog.
- Listen for a smooth fade with no step or click.
- `[stats] bt` shows no AVRCP volume command during the fade.
- The same on the speaker, in silent mode `z`.
- `Pl` through it: ~110, then ~50, then ~33 (screen off), then idle.
- Afterwards, Play pages and fades in.
- After the timer's pause, the headphones' play key (and a bud taken out
  and put back in): `[bt] headphones: play (ignored: paused, paused by the
  sleep timer)`, nothing plays; B plays.
- During the fade, with the screen off: a tap only wakes it, and the
  toast is up; the next tap on +10 min does nothing (`[screen] a touch on
  the glass: attended`, `[ui] sleep toast: the first touch after a wake
  from off ...`); the one after brings the music back over ~2 s, Turn off
  too; a tap elsewhere changes nothing. With the device in a pocket
  during a fade, the level never comes back up.
- With the "..." sheet open when the fade starts, a tap on the toast's
  +10 min acts (it doesn't only close the sheet); the sheet's Sleep
  timer row counts down while it is open, reads "Fading", then "Off".
- The "..." sheet's 4 rows are 40 px each, the sheet from y 40, clear of
  the red dots.
- End of album on the album's first track: the sheet's +10 min is dim and
  does nothing; on its last track it gives "what is left + 10 min".
- After the release, a B press within a second of it (before the
  `[bt] disconnected`): `[bt] let go while a play had just started:
  paused`, nothing plays, and turning the headphones on later doesn't
  start the music.
- The moon and "23 min" on Now Playing (alone in its line with the
  headphones' name), the tab bar's moon, the "..." row's state, the sheet's
  outline; `Tt` on a track with a known length fades its last 10 s and
  pauses on the next entry at 0:00.

### Step 5: idle power-off (item 4)

**Status: done in the code, host-tested (test_idle_policy; the
`flushNow()` cases in test_queue; the texts in test_ui_library); checked
on the device up to the `powerOff()` call (on USB, with the console's
"on battery" pretence), see "Device run: steps 4 and 5" below; the real
power-off on battery is still to do.** As built:

- `IdlePolicy` (lib/core): phases Blocked, Counting, Warning (the last
  30 s), Releasing and Off. `update(In)` every loop pass, after the
  player's (main.cpp's `stepIdle()`). What blocks it, in order: the setting
  Never; Playing; Waiting (a play waiting for the headphones); USB; the
  Pair screen's scan or a pairing (`BtLink` PairScan / Pairing, or
  `btSession.pairingUnderWay()`); the queue on its way to the card
  (`QueueStore::busy()`: a write under way, or an edit or move waiting its
  delay; not a write that failed and waits its retry, which with the card
  gone would keep it on forever); a screen of its own (calibration, a spike
  tool). A blocker, or input, restarts the countdown from that pass. So it
  counts from the pause, the sleep timer's included (section 3, step 6):
  nothing special is needed for it.
- **Input**: `ScreenControl::takeInput()` (a touch on the glass or the
  strip, a waking one too, and PWR), the pass's touch or button events, a
  headphone key that acted (Play, Pause on Bluetooth, Next, Prev, the
  volume keys; **not** a key that did nothing: a Play ignored after the
  sleep timer's pause, a Pause while already paused, a Next or Prev
  that only cues a track after the timer's pause, so a sleeper's in-ear
  detection doesn't keep it on: `HeadsetKeys::isInput()`), any console byte
  (`SerialConsole::poll()` returns whether something came), and the
  warning's Keep on.
- **USB** is the AXP192's power status (reg 0x00: ACIN bit 7 or VBUS bit
  5), which `ScreenControl` already read once a second for the wake on
  plug/unplug: `externalPower()`, with not-read-yet counting as present.
  The Core2's USB-C feeds ACIN (the probe's 5.13 V); VBUS counts too. It
  is read again right before `powerOff()`: plugged in during the release,
  it stays on (`IdlePolicy::cancel()`).
- **The warning** (added since the plan): for the last 30 s
  the toast "Turning off in 30 s" (Body, counting down) with **Keep on**
  (`Toast::showIdle`, `AppState::idleWarnS`). It replaces any toast up.
  A touch landing on it keeps it on (Keep on logs `[power] idle: Keep
  on`), and the rest of that touch is the toast's; a touch elsewhere keeps
  it on too and acts as usual. It doesn't light a dark screen (the spec's
  "it may be night"), and a wake is input anyway (it ends the warning).
  A lit screen stays lit while it is up (since batch 3: `holdLit`, item
  2; before, it could dim and go dark partway through, seen by hand).
  **So it is mostly seen with Screen off after set to Never**: the
  shortest idle length (10 min) is longer than the longest screen
  timeout (5 min), and everything that keeps the screen lit (a play
  waiting, a pairing, a screen of its own, USB) also blocks the idle
  countdown. With the other settings the device turns off with the
  screen dark and no warning, as the plan had it; the warning is for a
  screen left on (a desk, a dock without power). The exceptions: a test
  length (`Is<sec>`) shorter than the screen's timeout, and an event's
  wake (a failed track, the lost dialog) late in the countdown: an event
  isn't input.
- **At the end**, in order: `QueueStore::flushNow()` (below); the note
  (NVS "power"/"off_idle": the idle length in ms); the headphones let go
  the sleep timer's way (`releaseHeadphones("[power] turning off")`: the
  drop expected, no dialog, resting, still connectable, the output
  unchanged); then the policy waits until they are unlinked, at most
  3 s (`kReleaseWaitMs`), so they see a clean disconnect rather than a
  lost link. Input or a blocker in those 3 s cancels it: the note is
  cleared, the headphones stay let go (a play pages them). Then: haptics
  stopped, `Serial.flush()`, the panel's SPI lock, `M5.Power.powerOff()`
  (M5Unified: display sleep, the AXP192's power-off bit 0x32 bit 7, then
  `esp_deep_sleep_start()` with no wake source in case the PMIC didn't
  take it; it never returns).
- `QueueSaver` (lib/core): QueueStore's timing and piece-wise write moved
  here unchanged, over a `Store` (open queue.tmp, commit it over
  queue.txt, discard it, save the position); `QueueStore` is its card and
  NVS. `flushNow()`: a write under way is stepped to its end (all lines);
  if the queue changed since it began, the writer says so at once and it
  is dropped and written again whole; an edit not yet written is written
  without its 2 s or a retry's wait; then the position with the new
  generation. queue.tmp only replaces queue.txt complete, so a failed
  flush leaves the last good file (and the position is never paired with
  it).
- **The setting**: "Turn off when idle" on the Output tab after
  Brightness, drawn as the screen's rows (the value in a pill, a tap takes
  the next choice: 10 min, 20 min, 60 min, Never; a power symbol, faint at
  Never; the line "paused, on battery", or "stays on: more battery"). NVS
  "power"/"idle_after". A change restarts the countdown.
- **The boot toast**: `IdlePower::begin()` in setup reads and removes the
  note; once the UI is up, `Ui::note()` shows "Turned off after 20 minutes
  idle" (`IdlePolicy::offText()`: "1 minute", "45 s" for test lengths)
  for 6 s. Logged at boot too.
- **Console**: `I` status (`[power] idle: off after 20 min; counting, off
  in 1142 s`, or `waiting: on USB power`), `I<min>` or `Is<sec>` a test
  length until restart, `I0` the setting's again. The log: `[power] idle:
  waiting (...)` / `counting: off in N s ...` when that changes (not on
  every touch), `[power] idle: turning off in 30 s (...)`, `[power] idle:
  kept on (...)`, `[power] off after idle (20 min paused, on battery, no
  input): saving the queue, letting go of the headphones`, the queue's
  `[queue] saved now ...`, `[power] turning off: letting go of the
  headphones ...`, `[power] off now (...)`. For the bench (added for the
  device run): `Iu1` / `Iu0` tell the policy "on battery" while USB is in
  (until restart). The last-moment read in `stepIdle()` stays the real
  register's, so on USB the run ends in `[power] idle: USB power at the
  last moment: staying on` (the note cleared) instead of the power-off:
  everything up to the call is exercised, the call itself isn't. `Ib<sec>`
  leaves the note for the next boot's toast. Every console byte is input,
  so a test run must send nothing between its start and its end (a
  screenshot `X` ends the warning, but it reads the panel before the toast
  goes).

**Code:** `lib/core/IdlePolicy`, `lib/core/QueueSaver` with
`QueueStore::flushNow()`, `app/IdlePower` (the setting, the note, the
power off), `stepIdle()` and `idleCommand()` in main.cpp, the Output row,
the warning toast, `ScreenControl::externalPower()` / `takeInput()`.

**Host tests:** test_idle_policy covers:

- every condition that blocks it: playing, waiting, USB, Pair screen,
  pending write, input;
- the countdown restarting on input.

A queue test covers `flushNow()` in the middle of a piece-wise write.

As built: test_idle_policy (11 cases: the choices and the default; the
warning at 19:30 and the shutdown at 20:00; each choice and the console's
test length; every blocker held over twice the length, then the whole
length from when it clears; input restarting it; the warning ended by
input, USB or music, and its seconds; the release waiting for the
headphones, at most 3 s, and cancelled by a touch or USB; counting from
the pause after 90 min of music; a new choice restarting it; a random run
that it never shuts down while blocked or within the length of an input;
the texts). test_queue (6 cases: the saver's timing as QueueStore had it;
`flushNow()` in the middle of a piece-wise write; after an edit during the
write; an edit inside its 2 s and a position alone; a failed flush keeping
the last file, not busy, retried 10 s later; abort and markSaved for
remap). test_ui_library (the row's texts and every choice in its pill, the
warning and Keep on, the boot toast for each length).

**On the device:**

- A 2 min timeout from the console. The log shows `[power] off after
  idle`, the device is dark and silent, PWR boots it, and the queue and
  position come back, stopped.
- With USB in, it does not power off (and check what ACIN does to an
  off device).
- A power-off right after a queue edit loses nothing.
- With Screen off after: Never, the warning appears 30 s before, counts
  down, and Keep on (or any tap) keeps it on; with the screen off it stays
  dark.
- `Ts60` on the headphones with `I2`: 2 min after the timer's pause
  (before its own 5 min release) `[power] turning off: letting go of the
  headphones`, then `[bt] disconnected` with no "lost" dialog, then
  `[power] off now (headphones let go)` within 3 s.
- The boot toast "Turned off after 2 minutes idle" after a PWR boot.

### Device run: steps 4 and 5 (2026-09-27)

The working tree of steps 4 and 5 (plus the two fixes and the test hooks
below), flashed on the Core2 (COM3). On USB, the battery full, 240 MHz,
the speaker in silent test mode (`z`: volume 0), tracks from the card
(Graduation), the screen at Medium (100). **The Powerbeats never answered**
(pages at boot, then resting): everything that needs a link is still to do
(below). Screenshots: the scratchpad's `sleep_shots/` (LCD readback), all
looked at.

**Measured** (the P probe, USB mA, the mean of the 5 s windows between
`Pm` marks; one `Ts120` run: playing, then the fade, then the timer's
pause and screen off; differences under ~2.5 mA are noise):

| State | USB mA | Windows (sd) |
|---|---|---|
| Playing, screen bright (100) | 69.6 | 11 (3.8) |
| Playing, screen off (`Ps0`) | 58.0 | 11 (2.2) |
| Fading, screen bright | 71.3 | 6 (4.3) |
| Paused by the timer, screen off by the timer, amp off | **30.1** | 12 (1.2) |
| Paused, screen bright (for reference) | 44.5 | 12 (1.6) |

So the timer's end takes the speaker case from 69.6 (58.0 with the screen
already off) to 30.1: **-39.5 mA** (-27.9), the same as the steps 0-3
idle "off" (29.2) and paused-bright (44.0) within noise. The fade costs
nothing (a multiply): it reads as playing. (Playing here is 4 mA over the
steps 0-3 run's 65.5: another track, MP3, and noise.) The headphones' ~110
-> ~50 -> ~33 line needs the link.

**Sleep timer, checked on the device (log):**

- `Ts120` while playing: `the timer went off` at 225.4 s, the fade toast
  up at once; the level polled every 2 s (`T`): -1.3, -3.9, -6.6, -9.2,
  -11.9, ... -35.9, -38.6 dB, the level equal to the target within 0.1 dB:
  linear in dB, 1.33 dB/s, 40 dB in 30 s as specified. The pause 30.1 s
  after the expiry (the 30 s and the 100 ms at 0); `the pause is confirmed
  (silent): the fade back to 0 dB` and `the screen off` 310 ms later in
  the same pass (after `kSettleMs`), `[screen] bright -> off (the sleep
  timer)`; `[speaker] amp off` 2.0 s after the pause; `T` then reads
  `player paused, paused by the timer (headphone play ignored); the
  headphones are let go 5 min after the pause`.
- The release: 300.0 s after the confirmed pause (326.687 -> 626.710 on an
  End of track run), `[bt] release: not linked: the search rests` and
  `[sleep] 5 min paused: not linked: ...; the output stays the speaker`.
  A Core2 play (console space) before the 5 min: `[sleep] playing again:
  nothing more to do`, no release (three times).
- No AVRCP: on the speaker there is no link to send one to; nothing in
  the log mentions a volume command. The headphones' check is still to do.
- **+10 min** on the fade toast at -12.5 dB: `Sleep timer: 10 min left`,
  the level -3.8 dB 0.42 s later and 0 dB within 1.0 s (~20 dB/s, the
  slow rate). The same tap with the Sleep timer sheet open under the
  toast: it acts (the toast's buttons come first), the sheet stays.
  **Turn off** on the toast at -36.8 dB: `Sleep timer off`, then -32.2,
  -24.9, -16.2, -8.4, -0.2 dB at ~0.4 s steps, 0 dB after 2.0 s.
- **End of track** (the sheet's pill) with 4:31 known: `this track is
  the last: pausing at its end`; `fading out (the track's last 10 s)` 10 s
  before the end, then -0.4, -4.6, -8.6, ... -36.9 dB at 1 s steps (4 dB/s,
  linear in dB); `[queue] now at 7 of 27 (paused)`, `the fade ended`, the
  level -39.9 dB at the boundary; restored and the screen off 311 ms later;
  the next entry paused at 0.0 s.
- **End of album** on a track that isn't the album's last: `T+` gives
  `+10 min: refused (what is left isn't known)`, and the sheet draws +10
  min dim (screenshot).
- **Screenshots**: Now Playing with the moon and "23 min" beside "3 of 27
  · Speaker", and "10 min", "track", "fading" (amber); the tab bar's 7 x 7
  moon in the Now Playing cell (white; amber while fading, zoomed); the
  "..." sheet's 4 rows of 40 px from y 40 with "Sleep timer ... 23 min"
  first, clear of the red dots; the Sleep timer sheet (the title "Sleep
  timer: 22 min left", the pills, +10 min and Turn off in red), with "30"
  outlined in the accent after picking it, "End of album" outlined with +10
  min dim, and "Sleep timer: off" with the line "Fades out, pauses, then
  the screen goes off."; the fade toast "Sleep timer: fading [+10 min]
  [Turn off]" over Now Playing and over the open Sleep timer sheet.
- **Found and fixed: the Sleep timer sheet stayed up but unseen after a
  toast left.** +10 min on the fade toast over the open sheet, then the
  "Sleep timer: 10 min left" toast going: the screenshot showed Now
  Playing with no sheet, yet a later tap on "..." (289, 215) logged
  `[sleep] Sleep timer off`: it landed on the invisible sheet's Turn off.
  `Ui::uncover()` draws the header row again with `repaintHeader()`, which
  on a page with no header (Now Playing, the Dance tab) is the whole page,
  and only the 4-row "..." sheet was drawn again after it. It now draws
  every sheet up over such a page (the "..." sheet, the Sleep timer sheet,
  the volume sheet). After the fix the same sequence leaves the sheet drawn
  ("Sleep timer: 10 min left", +10 min and Turn off; screenshot), and `ui`
  (whose overlays line now includes `sleep timer sheet open/no`) says it is
  up.
- **Fixed: the log said "the timer went off" twice** for a timed choice
  (at the expiry and again when its fade ended: `Out::expired` is set for
  both). The second now reads `[sleep] the fade ended (...)`.
- Seen once: a BtAppT line and a loop line printed in the same moment
  share a line (`[bt] release: ... connectable)[sleep] 5 min paused: ...`,
  then an empty one): two tasks writing Serial. Cosmetic; left.

**Idle power-off, checked on the device (log; on USB with `Iu1`, `Is60`):**

- On USB as it is: `[power] idle: off after 20 min; waiting: on USB
  power`.
- With `Iu1` and `Is60`, the Screen off after set to Never (for the
  warning; set back to 30 s after): counting from the last console byte;
  `[power] idle: turning off in 30 s (the warning is up)` and the toast
  "Turning off in 30 s [Keep on]" 30.0 s later, counting down (screenshot
  at 23 s); a console byte (the screenshot's `X`, a scripted tap) keeps it
  on: `[power] idle: kept on (input)`, and it counts from there again.
- Left alone: at 60.0 s `[power] off after idle (1 min paused, on battery,
  no input): saving the queue, letting go of the headphones`, `[queue]
  saved now (the file was up to date) in 0 ms`, `[bt] release: not linked:
  the search rests`, `[power] turning off: not linked: ...; the output
  stays the speaker`, then 71 ms later `[power] idle: USB power at the last
  moment: staying on` and counting again: the order as specified, stopped
  by the real power status just before `powerOff()`.
- The boot toast: `Ib120`, a reset: `[power] the last power-off was the
  idle one: Turned off after 2 minutes idle` and the toast for 6 s once the
  UI was up (screenshot); the queue back at 7 of 27, stopped. The next
  reset: no toast (the note is read once).

**Still to do (the headphones, a finger, an ear, the battery):**

- Everything on the headphones: `Ts60` with the link (the fade by ear,
  smooth, no step or click; no AVRCP volume in `[stats] bt`; the stream
  suspended 3 s after the pause; the release at 5 min with no "lost"
  dialog; afterwards Play pages and fades in); ~110 -> ~50 -> ~33 mA; after
  the timer's pause their play key and a bud out and in: `(ignored: paused,
  paused by the sleep timer)`, B plays; a B press within a second of the
  release: `let go while a play had just started: paused`; the moon alone
  in the progress line beside "SPYDRONE".
- By hand (the scripted finger is console input and skips the wake latch):
  the pocket rule on the fade toast (the first touch after a wake from off
  acts on neither button); the warning's Keep on button itself (a scripted
  tap is console input, which ends the warning before the touch lands, so
  `[power] idle: Keep on` wasn't seen).
- On battery, USB out: a real idle power-off (`Is60`, or `I2` with
  `Ts60` on the headphones): dark and silent, PWR boots it, the queue and
  position back, the boot toast; with USB plugged in during the 3 s
  release it stays on; what ACIN does to an off device; a power-off right
  after a queue edit loses nothing.

### Step 6: CPU 160 by default, then the 80 MHz governor (item 6)

**Status: the setting is in the code (batch 3, below) and checked on the
device ("Device run: batch 3", after step 8). Step 6a ran: 160 failed the
scrolling check, so the default is 240 again (160 is the row's other
choice); 6b is deferred.**

**Step 6a: 160 by default?** Batch 3 made 160 the default
(`powerchoice::kDefaultCpuMhz`), for the soak to confirm, or for that one
constant to go back to 240. Run at 160 (the default, or the row):

- the 60-min Bluetooth soak (0 underruns);
- `w1`-`w3` scroll stress (fps, frame max, ring min);
- `b<n>` on MP3 and FLAC;
- `[dance]` fps with an MP3;
- `[audio] refill` times at a track start.

If they hold, 160 stays the default. **Result (2026-09-29, "Device run:
batch 3"):** the soak (0 underruns), the bench and the refills held, but
the scroll stress with an MP3 ran at about half of 240's frame rate: **the
default went back to 240.** **Device check:** `Pl` shows -5 USB
mA streaming (at 160, when chosen).

**As built (batch 3), the setting:**

- `PowerChoices` (lib/core): the choices, labels and lines, the stored
  value's check (`cpuMhzFromStored()`: 160 or 240, else the default),
  what a tap does (`cpuTap()`: a restart asked for, or saved alone when
  a `Pcb` since this boot means the other choice is what runs, or "wait"
  while a pairing is under way), when the restart goes
  (`cpuRestartDue()`: the headphones unlinked and the amp off, or 3 s),
  the dialog's title, the toasts, About's text.
- `app/PowerSettings`: NVS "power" ("cpu_mhz", "bt_tx", and "boot_cpu",
  the restart's note); `applyBootClock()` first thing in setup(); the
  restart (`Serial.flush()`, the LCD lock, `esp_restart()`). The boot log:
  `[power] CPU 240 MHz from boot (the default)` (or `160 MHz from boot
  (saved; Pcb0 goes back to the default)`).
- main.cpp: `MainUiHost::setCpuSpeed()` (refused during a pairing;
  saved; paused, the queue flushed, the note, the headphones let go, the
  speaker's amp asked off) and `stepCpuRestart()` (the restart once they
  are unlinked and the amp is off, at most 3 s).
- The Output row (`OutputPage::drawPowerSetting()`, `onCpuSpeed()`), the
  dialog (the Ui's dialog overlay), About's "CPU speed, Bluetooth power"
  row ("160 MHz; Normal (-12..+3 dBm)", the clock that runs).
- `PowerLab`: `Pcb` saves through PowerSettings (`Pcb0` the default, 240);
  `Pc` alone says what the next boot runs.

**On the device (to do):** the row's tap, Cancel (nothing saved: `Pc`
still says the same), Restart while playing on the headphones (paused,
`[queue] saved now`, `[power] restarting: letting go of the headphones`,
`[bt] disconnected` with no "lost" dialog, `[power] restarting now at 160
MHz` within 3 s), the boot toast "CPU speed: 160 MHz", the queue at the
same entry, stopped; nothing plays until asked. Restart while playing on
the speaker: no pop or click through the reset (the amp off first:
`[power] restarting now ... (headphones let go)` with no "amp still on");
the toast stays up until the screen goes dark. A tap during a pairing
(Pair screen, a device picked): "Wait for the pairing to finish", nothing
saved. `Pcb240` at 160: the
pill says 240 MHz and the line "160 MHz until a restart"; a tap saves 160
back, with no dialog and no restart.

**Step 6b: the idle governor. Deferred** (item 6): the idle power-off
already caps idle time on battery. A CpuGovernor that switches 160 and 80.
Host tests cover when it may drop. **On the device:**

- linked and paused, toggle every 5 s for 30 min: 0 disconnects, and
  AVRCP keys still work;
- speaker pitch is right after a switch;
- `Pl` shows -4 USB mA idle.

### Step 7: TX floor (item 7)

**Code:** `esp_bredr_tx_power_set(N12, P3)` at stack start. **As built
(batch 3):** the "Bluetooth power" setting (Low 0..2, Normal 0..5, High
0..7; item 7), its levels set by `PlayerA2dp::bt_start()` right after the
controller is enabled, before Bluedroid and so before the first page, and
at once on a change (`BtSink::setTxPower()`). `BtLinkLevel` (lib/core)
keeps the row's "From the next connection" while the link that is up
has another choice's levels.

**On the device:**

- 30 min streaming with the Core2 in a pocket and at 5 m: underruns and
  `gap=` unchanged;
- `Pl` A/B against the default, each after a fresh connection;
- `Pt4,5`, then a tap on the row, then `P`: the line "Pt's ... test
  replaced by the Bluetooth power row" and `tx=` with the row's range, not
  `(Pt)`.

### Step 8: Dance frame rate (item 8)

**Code:** 10 fps idle, 24-30 fps dancing.

**Status: in the code (batch 3, item 8's "As built"); host-tested
(test_dance_rate); checked on the device ("Device run: batch 3", below),
except the click tracks' beat error and the stick figure.**

**Device check:**

- `[dance]` fps and beat error on the click tracks unchanged while
  dancing: `fps=~30/30` at 240 MHz, `~24/24` at 160, with
  the error medians of MASCOT-POC.md;
- an MP3 at 160: a steady `~24/24`, where 30 landed at 20-24;
- a pause: `[dance] 10 fps (idle)` within ~0.3 s (the fade), a play:
  `[dance] 24 fps (dancing at 160 MHz)` once it locks; the crab's
  breathing and blinking still read fine at 10;
- `Pl` on the Dance tab paused, 30 vs 10 fps (expect ~-3 USB mA): `k0`
  (a frozen pose draws at the dancing rate) against `k` (idle, 10);
- the screen off on the Dance tab: `[dance] off`, and on the wake the
  rate logged again.

### Device run: batch 3 (2026-09-29)

The working tree of batch 3 (steps 6-8, plus the two fixes below),
flashed on the Core2 (COM3). On USB, the battery full, the screen bright
at Medium (100; Screen off after set to Never for the run, 30 s again
after). **The Powerbeats answered this time**: streaming is `tone:silence`
over Bluetooth (`Pz`), at the default volume (30%); anything with music
(the Dance tab) is on the speaker in silent test mode (`z`, volume 0),
with the headphones disconnected (160) or linked but idle (240). USB mA,
the mean of 13 five-second windows (65 s) after a `Pm` mark, each state
settled 15 s or more. Screenshots: the scratchpad's `batch3_shots/`
(LCD readback), all looked at.

**Measured:**

| State | USB mA | Notes |
|---|---|---|
| Streaming, 160, Normal | 99.1, 106.8, 98.5 (mean 101.5) | three connections: the boot's, then two fresh ones |
| Streaming, 160, Low | 97.9, 95.1 (mean 96.5) | each after a fresh connection |
| Streaming, 160, High | 109.9, 102.0 (mean 106.0) | each after a fresh connection |
| Streaming, 240, Normal | 102.6, 102.0 (mean 102.3) | the boot's connection, then a fresh one |
| Dance tab paused, 160, idle crab (10 fps) | 42.3, 39.4 | speaker, headphones disconnected; A/B/A |
| Dance tab paused, 160, frozen pose `k0` (24 fps) | 42.4 | the same |
| MP3 on the speaker, 160: Now Playing / Dance tab dancing | 54.0 / 56.7 (+2.7) | ~17 fps dancing (below) |
| MP3 on the speaker, 240: Now Playing / Dance tab dancing | 59.2 / 63.0 (+3.8) | ~25 fps dancing; Now Playing without its 2 spike windows (81, 108 mA) |

The order was Normal, Low, High, Normal, Low, High, Normal (160), then 240.
**The spread between connections at the same setting is as large as
the differences** (Normal 98.5 to 106.8), so:

- **Bluetooth power:** the order Low < Normal < High holds on average
  (-5.0 and +4.5 against Normal) but isn't resolved. Headphones on the
  desk, a metre or so away.
- **CPU 160 vs 240 streaming:** -0.8 (the means) or -3.2 (the medians):
  not resolved here. Section 1's -5.2 stays the reference. The MP3 on the
  speaker reads -5.2 at 160 (54.0 against 59.2), but with the headphones
  linked (idle) only in the 240 run.
- **The idle crab (10 fps) against 24 fps:** paused at 160 the difference
  is within noise (≤~1.5 mA). The planned -3 mA was for 30 against 10 at
  240.
- **The paused Bluetooth link is a noisy baseline:** with the headphones
  linked and paused, some windows jump to 66-92 mA (sd 17-19) with nothing
  else changing. The Dance A/B on the link was unusable, so it was redone
  on the speaker with them disconnected.

**Dance frame rate, checked (log):**

- Paused: `fps=10.0/10 (idle)`, draw 2.9 ms and push 4.4 ms. With `k0`,
  `fps=23.7-24.0/24 (dancing)`.
- **Playing an MP3 (Air, "Kelly Watch The Stars") on the muted speaker,
  the target isn't reached**: at 160, `fps=16.0-17.4/24`, draw 12-17.5 ms
  and push 11-15 ms; at 240, `fps=24.2-25.2/30`, draw 5-7.6 ms and push
  10.7-12 ms. A FLAC ("Stronger") at 160 ran 13-15/24 in its first seconds.
  So while the decoder runs, a frame takes 25-30 ms at 160 (the push
  doubles too: the card's reads share the SPI bus). The pacer doesn't
  burst, it just runs slower. Item 6's "20-24 at 160" was an MP3 over
  Bluetooth, not the speaker.
- A pause: `[dance] 10 fps (idle)` 0.42 s after it (the fade). A lock:
  `[dance] 24 fps (dancing at 160 MHz)` (at 240, `30 fps (dancing at 240
  MHz)`). A lost beat: `10 fps (idle)`, and 24 again on the relock. Before
  the first lock of a track it can flip 24, 10, 24 within 0.5 s, as the
  pose weight crosses 0.5.
- The screen off on the Dance tab: `[dance] off`. Back on: `[dance] on
  (crab)` and the rate logged again, awake in 76 ms.

**CPU speed and Bluetooth power, checked (log and screenshots):**

- **Boot, every time:** `[power] CPU 160 MHz from boot (the default)` (or
  `(saved; Pcb0 goes back to the default)`), `[power] bluetooth power:
  Normal (-12..+3 dBm)`, and `[bt] tx power: -12..+3 dBm (levels 0..5),
  from the stack's start` 0.15-0.2 s after them, ~10 s before the first
  page. `Pc`: `CPU 160 MHz (PLL 320 MHz); from boot: 160 MHz (the
  default)`. `Pcb0` at the start: nothing was saved.
- **The rows** (screenshots): "CPU speed / Saves battery, a bit slower /
  [160 MHz]" and "Smoothest lists, dancing / [240 MHz]"; "Bluetooth power"
  with 2, 3 or 4 bars lit: "Saves battery; stay close / [Low]", "Adjusts to
  the distance / [Normal]", "More range, more battery / [High]" (all
  unlinked or on the link's own choice). While linked after a change: "From
  the next connection" (High and Low). Back on Normal, the link's choice,
  it reads "Adjusts to the distance" again. Every text fits.
- **The dialog** "Restart at 240 MHz?", with the body text, [Cancel]
  [Restart] (screenshot). Cancel: nothing logged, `Pc` unchanged, the row
  unchanged.
- **Four restarts**: 160 -> 240 on the headphones (streaming), 240 -> 160
  on the speaker (an MP3 playing, muted, the headphones linked), then both
  again after the fixes:
  - `[power] CPU speed: 160 -> 240 MHz (saved): restarting (paused first);
    saving the queue, letting go of the headphones` (`, the speaker's amp
    off` on the speaker), `[queue] saved now`, `[bt] release: letting go`.
  - After the fixes, on the headphones: `[bt] A2DP down (closed)` 171 ms
    later, then `[power] restarting now at 240 MHz (headphones let go)`.
    On the speaker: `[speaker] amp off: I2S stopped, AXP192 GPIO2 low
    (asked: Pa)` 62 ms after the tap, `A2DP down` 1.2 s after it, then the
    restart.
  - The next boot: `[power] restarted for the CPU speed: 240 MHz asked`,
    `Last reset software`, and the toast "CPU speed: 160 MHz" over Now
    Playing (screenshot).
  - The queue at the same entry, `(stopped)`. On the headphones,
    `stream=suspended`, nothing played. They reconnected by themselves 10 s
    after the boot (once on the second page, ~20 s) and took the output, as
    at any boot. The speaker's volume came back as it was (30%): silent
    mode doesn't outlive a restart.
  - **"Your place" is the queue entry:** the silence track was 33 s in and
    started again from 0:00. QueueStore saves only the entry, at any boot.
    The dialog's "the queue and your place are kept" may read as more.
    (Since fixed: "The resume point", below.)
- **`Pcb240` at 160:** the pill reads "240 MHz", the line "160 MHz until a
  restart" (screenshot). A tap: `CPU speed: 240 -> 160 MHz (saved): it
  runs at that already, no restart`, no dialog.
- **Bluetooth power readback:** after each fresh connection `Pt` reads the
  choice (`levels 0..2`, `0..7`, `0..5`; the setting's label beside it).
  Right after a change while linked it already reads the new levels, so
  the readback shows the controller's setting, not the link's level.
  (PowerLab's comment said it read back the old range with a link up; not
  seen this run. The comment is fixed since.)
- **`Pt4,5` then a tap on the row:** `Pt ... asked: applies from the next
  connection`, `Pt` reads `levels 4..5 ... (Pt, a test until restart)`. The
  tap: `Normal -> High (saved)`, then at once `Pt's +0..+3 dBm test
  replaced by the Bluetooth power row: High (-12..+9 dBm)`; the next P line
  `tx=-12..+9 dBm (High)`.
- **About:** "CPU speed, Bluetooth power / 160 MHz; Normal (-12..+3 dBm)"
  (screenshot).

**Found and fixed:**

- **The restart never waited.** `loop()` takes `now` at the start of its
  pass, and the dialog's Restart stamps `cpuRestartAskedMs = millis()` later
  in the same pass. So `stepCpuRestart(now)` computed `now - asked` < 0,
  which as a uint32 is ~49 days, and `cpuRestartDue()` restarted at once.
  The speaker restart logged `(the headphones still linked after 3 s; the
  speaker's amp still on after 3 s)`, with every line from the tap to the
  ROM banner in one serial read. `cpuRestartDue(nowMs, askedMs, ...)` now
  takes both times and counts a negative elapsed as 0. A host test covers
  the case and a millis() wrap.
- **The release wait ended as the disconnect began.** `BtSink::connected()`
  is the library's `is_connected()`. It goes false on the stack's
  DISCONNECTING, right after `esp_a2d_source_disconnect()`, where a clean
  disconnect takes 0.15-1.5 s (the Disconnect button in this run). The
  first restart logged `(headphones let go)` with no `A2DP down`.
  `BtSink::linkUp()` (the loop's link: up from CONNECTED until
  DISCONNECTED) now gates the CPU restart and the idle power-off's release
  wait (`IdlePolicy`'s `linked`, the `[power] off now` line). PlayGate
  keeps `connected()`.

**Seen, not changed:**

- The first boot after flashing (an RTS reset from the old firmware) found
  no IMU (`[diag] IMU none found`, `imu=on` in P lines). Every later boot,
  at 160 and at 240, found the BMI270 and suspended it. So it isn't the
  clock; probably the IMU's state from before the flash.
- The screenshot (`X`) holds the Bluetooth output while it reads back
  (~17 s: `pos` stuck, `bt=0fps`, then `gap=120ms`). This is the test tool;
  no screenshot was taken during a measured window.
- "bt_tx" has no console command to remove it. After the run it is stored
  as Normal, which acts the same as absent. "cpu_mhz" was removed with
  `Pcb0`.
- The Output list scrolls a different distance with the card linked or
  not (the card's height). A scripted tap once opened Touch calibration;
  it was closed with `aq` and the table was left unchanged (`as`: the
  default).

**Step 6a: 160 against 240, and the default (later the same day).**
The same batch 3 build, then with the default changed (below). Everything
over Bluetooth to the Powerbeats at the default volume (30%), USB power,
160 and 240 each from a boot (`Pcb0` / `Pcb240`, then a reset), the same
files at both speeds. MP3: Daft Punk, *Discovery* (added to the queue for
the run and removed after); FLAC: Kanye West, "Stronger", and Emancipator,
"Alligator".

*The scroll lab's stress* (`w1` governor on, `w2` off, `w3` off and no
cap; 30 s each, `wd30`; the build's defaults: `wm1` hardware scroll, `wp1`
paced refill, 30 fps cap, flicks at 2,000 px/s; the 10,000-track synthetic
library, `g10000`, artists view with the A-Z rail). fps is while moving,
p10 / p50 / min; frame is the per-second means' p50 and the longest; ring
min is the lowest steady ring fill. No underruns in any run; heap min
60.2-62.7 K at both speeds.

| Run | 160 MHz: fps | Frame p50 / max | Ring min | Decode p50 / max | 240 MHz: fps | Frame p50 / max | Ring min | Decode p50 / max | fps p50, 160 vs 240 |
|---|---|---|---|---|---|---|---|---|---|
| MP3, `w1` | 6.7 / 7.8 / 6.4 | 108 / 181 ms | 1,413 ms | 55 / 63 % | 12.9 / 15.9 / 12.8 | 46 / 115 ms | 1,410 ms | 42 / 47 % | **-51 %** |
| MP3, `w2` | 6.6 / 7.8 / 6.3 | 106 / 179 ms | 1,413 ms | 54 / 63 % | 13.8 / 15.7 / 12.8 | 46 / 105 ms | 1,410 ms | 42 / 47 % | -50 % |
| MP3, `w3` | 6.8 / 8.2 / 5.6 | 101 / 178 ms | 1,410 ms | 54 / 62 % | 13.6 / 17.9 / 12.0 | 43 / 105 ms | 1,410 ms | 43 / 46 % | -54 % |
| FLAC, `w1` | 12.7 / 15.5 / 11.3 | 47 / 102 ms | 1,410 ms | 36 / 40 % | 21.4 / 24.7 / 19.7 | 24 / 81 ms | 1,410 ms | 30 / 33 % | **-37 %** |

- 240 matches the earlier reference: UI-SPIKE.md's `w1` `wf30` `wm1` at
  2,000 px/s with an MP3 (240, the muted speaker) read 14.5 / 16.5 fps.
- At 160 a frame costs 2.3x as long with an MP3 (draw 55 against 19 ms,
  push 47 against 25): the decoder takes 55 % of core 1 instead of 42 %
  and preempts the loop mid-frame, and the SPI bus waits grow with it.
  Uncapped (`w3`) doesn't help: the frame cap isn't the limit at 160.
- The ring doesn't notice at either speed: the audio holds, the list
  doesn't.

*Decoding bench* (`b<n>`: 20 s of audio decoded flat out, output
discarded):

| File | 160 MHz | 240 MHz |
|---|---|---|
| MP3, "One More Time" | 3.1x realtime (32.3 % of a core) | 4.3x (23.2 %) |
| MP3, "Harder, Better, Faster, Stronger" | 3.0x (33.7 %) | 4.1x (24.3 %) |
| FLAC, "Stronger" | 3.6x (27.8 %) | 4.4x (22.9 %) |
| FLAC, "Alligator" | 3.8x (26.2 %) | 4.6x (21.9 %) |

At least 3x realtime at 160: the bench holds.

*`[audio] refill` at a track start* (ms after the request: first audio /
500 ms buffered / 1,000 ms, steady / full; `wp1` pacing at 1.5x from 500
ms):

| Start | 160 MHz | 240 MHz |
|---|---|---|
| MP3, from stopped (`i`) | 30 / 204 / 546 / 1,157; 38 / 260 / 609 / 919 | 25 / 157 / 505 / 928 |
| MP3, a skip while playing (`n`, 3 each) | 34-43 / 554-596 / 1,653-1,715 / 2,615-2,698 | 29-38 / 333-348 / 1,379-1,418 / 2,370-2,371 |
| FLAC, from stopped | 46 / 275 / 2,284 / 4,258 | 39 / 218 / 1,773 / 3,190 |
| FLAC, a skip (2 each) | 50-51 / 249-322 / 2,040-2,088 / 3,585-3,954 | 46-47 / 206-249 / 1,660-1,665 / 2,950-3,100 |

Slower at 160 (an MP3 skip reaches 500 ms buffered ~240 ms later, full
~0.3 s later), no underruns, time to first audio within ~10 ms.

*The Dance tab with an MP3 over Bluetooth* ("Harder, Better, Faster,
Stronger"): 160 `fps=16.8-19.7/24`, draw 9-16 ms, push 8-14 ms; 240
`fps=25.3-27.3/30`, draw 4.5-7 ms, push 8-14 ms.

*The 60-minute Bluetooth soak at 160* (the default then, a fresh boot;
`Pz`, an hour of `tone:silence`; the screen off after 30 s as normal; 718
stats lines, 13:16-14:16): **0 underruns**; `gap=24ms` in every line; the
ring 1,433-1,486 ms (p50 1,471); the pull rate 43,087-44,609 frames/s (p50
44,312); internal RAM min 71 K; no disconnects, no `[bt]` events. At the
end of the hour the queue moved on to the next entry, a Daft Punk MP3,
which played at 30% for ~50 s over the headphones before it was paused
(the test's queue order: silence was followed by the MP3s added for the
run). Its fill after the change: 500 ms at 536 ms, full at 2,580 ms, no
underruns.

**The decision: the default goes back to 240.** The rule was: 160 stays
only if the soak has 0 underruns, the scroll stress's fps is within ~15 %
of 240 with a healthy ring minimum, and the bench holds. The soak (0
underruns) and the bench (3.0x realtime or more) pass; the ring minimum
is healthy (1,410 ms at both speeds); **the scroll fps is 51 % lower with
an MP3 (7.8 against 15.9) and 37 % lower with a FLAC (15.5 against 24.7)**,
far outside ~15 %. The Dance tab with an MP3 (~18 of 24 against ~26 of
30) and the track-start fill point the same way. So
`powerchoice::kDefaultCpuMhz` is 240 again. 160 stays on the Output tab
("Saves battery, a bit slower", ~5 USB mA streaming; since then "Slower
lists, saves a little", and its dialog says so: "The 160 MHz texts and
the held screen" below) for anyone who prefers battery to smoothness.

- Changed: `lib/core/PowerChoices.h` (`kDefaultCpuMhz = 240`, with the
  reason), test_power_choices (the default, and an invalid stored value's
  label, now 240), comments in `PowerLab.h` (`Pcb0`: 240) and
  `DanceRate.h`; this file (item 6, step 6, the battery table's note) and
  ARCHITECTURE.md (the CPU speed section, the boot log, `Pcb`).
- `pio test -e native`: 633 of 633 pass. `pio run -e core2`: builds (RAM
  53,672 B, flash 2,135,019 B, unchanged). Flashed; the boot log reads
  `[power] CPU 240 MHz from boot (the default)`, `[power] bluetooth power:
  Normal (-12..+3 dBm)`, `[bt] tx power: -12..+3 dBm (levels 0..5), from
  the stack's start`; the queue came back at 8 of 27, stopped; the
  headphones reconnected on the first page and nothing played.
- "cpu_mhz" is absent (`Pcb0` before the last 160 boot), so the device
  now runs the new default. A device that saved 160 (the row or `Pcb160`)
  keeps it: the explicit choice wins.

**Still to do:**

- The Bluetooth power range soak (Low at arm's length and in a pocket;
  underruns, `gap=`).
- By ear: a restart while the speaker plays out loud, no pop. Here the
  speaker was muted; the log shows the amp off before the reset.
- A tap during a pairing ("Wait for the pairing to finish").
- The "Restarting at 160 MHz..." toast on screen: it is up only until the
  reset (≤1.5 s here), too short for the readback. The log has it.
- The click tracks' beat error at 160/240; the stick figure's rates.
- Whether the Dance tab should aim lower while the decoder runs (it gets
  ~17 of 24 at 160 with an MP3 on the speaker).
- On battery: everything in step 9.

### The resume point (after batch 3)

The user asked for the CPU speed's restart to pick up at the same second,
as its dialog promised. As built (host-tested; the on-device checks
below are still to do):

- **What is saved:** NVS "queue"/"resume", one blob (the queue file's
  generation, the entry's line, its path's FNV-1a hash, the ms, the length
  then), at every pause and so at every orderly shutdown: the CPU speed's
  restart (paused first), the idle power-off (only paused or stopped), the
  sleep timer's pause. It is removed when playback moves on: a play, a
  skip, another entry, an edit that changes the current entry (one that
  only moves it saves it again at its new line). Nothing while playing:
  no writes every second, and a power cut while playing starts the entry
  at 0:00, as before. `QueueSaver` decides (test_queue), `QueueStore`
  writes it; like the position, it pairs with the file of its generation.
- **At boot:** a point saved for the restored file's current line, whose
  track still has that path, becomes the player's start point
  (`PlaybackController::setStartPoint()`, test_playback). The player comes
  up stopped: nothing plays. Now Playing and the tab bar show that second
  and the saved length; play starts there, faded in as any start.
  Next, another entry, or an edit that changes the current entry drops it;
  prev on it goes to 0:00 of the same entry. (The design said "as prev
  does after 3 s of play today": prev didn't do that yet then. It does
  now, ARCHITECTURE.md "Transport": past a track's first 3 s prev restarts
  it, paused it stays paused; on a waiting start point, whatever the
  second, it goes to 0:00 and starts nothing, stopped staying stopped.) It
  applies after any boot with one saved, the power key while paused too.
- **Where it starts** (`TrackSeek`, test_track_seek; ARCHITECTURE.md,
  "Audio pipeline"): an MP3 from its Xing or VBRI TOC, else its average
  or its first frame's bitrate, on a clean frame, without the ID3 reader;
  a FLAC through libFLAC's own seek (a subclass reaches the decoder; no
  change under .pio/libdeps); a built-in track counts from there. (Since
  the review below: a headerless VBR MP3 by the saved length, and a byte
  that doesn't check out starts at 0:00.) The
  last 5 s and past the end start at 0:00 (chosen over "treat as ended":
  nothing is skipped by itself); a file whose track is gone or renamed
  doesn't match and starts at 0:00; a file replaced under the same name
  starts at the second if it is long enough. A 48 kHz file on Bluetooth
  is still refused. `positionMs()` counts from the start, and the
  read-rate length estimate adds it.
- **Texts:** the dialog's body (UiText's `kCpuDialogBody`, 3 lines of
  Small, measured in test_ui_library; to 160 it is `kCpuDialogBody160`
  since, with the same resume wording: the next section); the boot toast
  "CPU speed: 160 MHz" is unchanged.
- **Console:** `q` / `l` print the saved point and a waiting start point;
  `qs<sec>` starts the current entry that far in (playing: now), `qs0`
  clears it; `[queue] resume point saved: 1:23 into 5` / `cleared`, and
  `[audio] MP3: starting 1:23 in, of 5:20 (Xing TOC): byte ..., a frame
  +N` or `[audio] FLAC: starting 1:23 in (libFLAC's seek, N ms)`.
- `pio test -e native`: 647 of 647 pass. `pio run -e core2`: builds (RAM
  53,720 B, +48; flash 2,143,147 B, +8,128: libFLAC's seek is linked now).

**To check on the device** (what was checked, and what is still open:
"Device run: batch 3 follow-ups", below):

- Accuracy with `qs<sec>` against the file's own time (a player on the
  PC): a CBR MP3 and a FLAC within ~0.5 s; a LAME VBR MP3 (Xing TOC)
  within 1-2% of its length; a VBR file without a TOC (if the card has
  one). The log's byte and frame lines for each.
- The FLAC seek's time (`libFLAC's seek, N ms`) on a long FLAC without a
  SEEKTABLE, and the decode task's `stack_free` after seeks (the seek adds
  libFLAC's bisection and a few small buffers to its stack).
- The CPU restart both ways, playing on the headphones and on the
  speaker: after the boot Now Playing shows the second, nothing plays,
  play picks up there (by ear: no burst of the track's start, the fade-in).
- A pause, then the power key off; a pause, then the idle power-off
  (`Is<sec>`, `Iu1`): the second after the boot. A power cut while
  playing: 0:00. Prev and next on a waiting point.
- A big ID3 tag (the Moon Safari tracks) resumed mid-way.

### The 160 MHz texts and the held screen (after batch 3)

Two more of the user's fixes from the batch 3 run, host-tested; the
on-device checks below are still to do.

- **The 160 MHz texts** (item 6). The row's line was "Saves battery, a
  bit slower"; measured, 160 halves list scrolling with an MP3 playing
  (~8 against ~16 fps), the Dance tab runs ~18 of 24 fps, and the saving
  didn't show above the batch 3 run's noise (the audit's -5.2 USB mA
  streaming). Now "Slower lists, saves a little" (UiText's `kCpu160Sub`,
  167 of 174 px; "Half-speed lists, less power" is 180 px, too wide). To
  160 the dialog's body is "Saves a little battery; lists scroll at half
  speed while music plays. Music pauses and picks up at the same second."
  (`kCpuDialogBody160`, 3 lines of Small: 256, 255 and 218 of 260 px);
  `powerchoice::cpuDialogBody()` picks it, and to 240 the body stays "The
  speed changes at a restart. The music pauses and picks up at the same
  second." 240's line stays "Smoothest lists, dancing". Tests:
  test_power_choices (which body), test_ui_library (both bodies fit, under
  the dialog's 128 bytes).
- **The held screen** (item 2). The idle power-off's warning let a lit
  screen dim 1 s after it appeared and go dark 10 s later, 20 s before
  the power-off (seen by hand): the screen's countdown ran on under it.
  `ScreenPower::step()` has a third input, `holdLit`, which main.cpp sets
  while `IdlePolicy` is in Warning or the sleep timer's fade counts down
  to the pause (`fadeCountingDown()`, since the review below):
  - a lit screen (Bright or Dim) goes Bright (`Why::HoldLit`, "held lit
    for a countdown toast") and stays so until it ends; the countdown
    then runs from its end, so Keep on (input) gives the whole timeout
    from the tap;
  - an Off screen stays off (the night rule), and the sleep timer's
    screen off at its pause still turns a held screen off;
  - a screen woken during it is held from the wake: an event's wake at
    once; a touch's or PWR's wake from Off keeps the pocket guard (off 10
    s later) until input follows, then is held. For the idle warning that
    wake is input itself: `ScreenControl::takeInput()` ends the warning in
    the same pass (stepIdle runs before the screen's step), so the screen
    follows the guard and then the normal timeout. The swallowed wake
    and the pocket rule are untouched;
  - `keepLit` still wins (it lights an Off screen).

  `ScreenControl::step()` logs `[screen] held lit while the toast counts
  down` and `[screen] the toast is gone: the countdown again (dims in
  N s)`; the warning's line says `the warning is up, and the screen stays
  lit until it ends`. Tests: test_screen_power (3 new: a lit screen held
  and the countdown after, a dim one brightened, Never; an off one left
  off, the timer's screen off, keepLit; the wakes during it).
- `PowerLab`'s `Pt` comment now says what the device showed: the
  controller reads the new range back at once while linked; it is the
  controller's setting, and the link keeps its level until the next
  connection.

**To check on the device** (what was checked, and what is still open:
"Device run: batch 3 follow-ups", below):

- The row's line at 160 and the dialog to 160 (screenshots: 3 lines, none
  cut); the dialog to 240 unchanged.
- The idle warning on a lit screen (Screen off after 30 s, `Iu1`, a tap,
  then `Is40`: the warning comes up ~10 s in, while the screen is still
  bright): it stays bright to the power-off (on USB: "USB power at the
  last moment", then the countdown again: dims 20 s later). From Dim (a
  tap, then `Is55`: the warning ~25 s in, the screen dim from 20 s):
  bright again at once, and Keep on acts on the first tap. The screen off when it appears: stays off (a tap then wakes
  it, swallowed, and keeps the device on: the pocket guard's 10 s, then
  dark).
- The sleep fade on a lit screen (`Ts40` with Screen off after 30 s: the
  fade starts 10 s in): lit through the fade, off at the pause. Woken by
  a tap during the fade (from off): off again 10 s later unless tapped
  again; a second tap holds it to the end.

### Review fixes (the resume point and the held screen)

A review of the two changes above found six real faults; each is fixed
and host-tested (`pio test -e native`: 655 of 655, 5 new).

- **A VBR MP3 without a Xing or VBRI header** was placed by its first
  frame's bitrate: a silent 32 kbit/s first frame put 3:00 of a 4:00
  file at about 0:30 while Now Playing said 3:00. The start point's
  length now goes to the backend with the play (`play()`'s
  `durationHintMs`, from `PlaybackController`; it was the catalog's hint,
  0 for files, and the backend ignored it). `TrackSeek` checks the frames
  in its 4 KB read: if they differ in bitrate, or the handed length
  differs from the first-frame one by over 3%, the start is placed by the
  average bitrate over the handed length; a plain CBR file whose length
  agrees keeps the exact first-frame bitrate; a VBR file with no length
  handed starts at 0:00 (`[audio] MP3: 3:00 asked: VBR, no table of
  contents, no length: from 0:00`). test_track_seek.
- **A FLAC with an ID3v2 tag in front** never resumed: its STREAMINFO
  was read at byte 0, so it had no rate and the seek wasn't tried (and
  the log blamed libFLAC). The backend now reads STREAMINFO past the tag,
  as libFLAC does; such files also get their length on Now Playing. A
  FLAC with no STREAMINFO found logs that instead.
- **A start byte that didn't check out was used anyway**: in a file
  shorter than its header says, the byte clamped to the file's last one,
  the decoder ended at once and the player moved on to the next entry.
  No clean frame in the 4 KB read, or 5 s or less of audio after it at
  its bitrate (`trackseek::mp3MsLeft()`), is now a failed seek: from
  0:00, as a FLAC seek that fails.
- **A dropped start point could come back**: removing the current entry
  (or clearing the queue) only hid it behind the entry's key, so an undo
  followed by prev or a tap on that entry started it at the old second.
  `PlaybackController` now drops it whenever the current entry changes
  under it (`currentMoved()`, `remove()` of the current entry,
  `clearQueue()`, `playNow()`); `QueueStore::remap` still carries it
  across a rebuild. test_playback.
- **The fade toast's hold could last a whole track or album**: after a
  skip during a track's fade (End of track, album, queue) the timer stays
  Fading, with its toast, until the new track's last 10 s or the album's
  end, and `holdLit` kept the screen bright all that time.
  `SleepTimer::fadeCountingDown()` is true only for a timed fade or a
  track's fade in the boundary track's last 10 s; main.cpp holds the
  screen by it. test_sleep_timer.
- **`qs<sec>` dropped the track's length** (Now Playing showed `--:--`
  and a dotted bar, and the saved point had no length):
  `setStartPoint(ms, 0)` now takes the length as known, a waiting start
  point's, the held track's (paused or playing), else the catalog's hint.

`pio run -e core2`: builds, no warnings (RAM 53,728 B, +8; flash
2,144,595 B, +872). Nothing new runs on the decode task's stack beyond a
42-byte re-read and a scan of the 4 KB PSRAM buffer already read.

**To check on the device** (what was checked, and what is still open:
"Device run: batch 3 follow-ups", below):

- A VBR MP3 without a Xing frame, if the card has one (or one made with
  `ffmpeg -write_xing 0`): paused mid-way, the CPU speed's restart (the
  Output tab) or a power key off: the log says `average bitrate` and the time heard matches the
  time shown within a few percent. `qs<sec>` on it while stopped with
  nothing played since the boot: `from 0:00`.
- A FLAC with an ID3v2 tag in front (`metaflac` won't make one; some
  taggers do): its length on Now Playing, and `qs<sec>` resumes there.
- End of track with the screen lit, a skip during the last 10 s: the
  toast stays, the screen dims and goes off on its usual timeout.
- `qs120` on a paused track: Now Playing keeps its length.

### Device run: batch 3 follow-ups (2026-09-29)

The three fixes above (the resume point, the 160 MHz texts, the held
screen) with the review's fixes, flashed on the Core2 (COM3). On USB, the
speaker in silent test mode (`z`); the headphones were on the table and
never answered their pages, so nothing ran over Bluetooth. The queue was
lengthened for the run (Moon Safari, Discovery, then `tone:silence` last)
and put back after. Screenshots (LCD readback, all looked at): the
scratchpad's `resume_shots/`.

**How the accuracy was measured.** A temporary console command (`qe`,
removed after the run) dumped the speaker tap's level, RMS per 10 ms, each
block labelled with the time the device claims (`startOffsetMs()` plus
the frames played). On the PC each file was decoded on the device's
timeline (ESP8266Audio's libmad built for the PC, `devmad`; libsndfile for
FLAC), and each run was aligned to it at the sample. A run from 0:00 of
every file matched the PC decode at 0.0 ms, so the reference is the
device's own timeline, Xing frame and decoder delay included. Error = the
time shown minus the time heard (negative: the audio is behind the
display).

| File | Kind | Placed by | Error at 1:00 / 2:30 (others) |
|---|---|---|---|
| Air, "Kelly Watch The Stars" | CBR 192, LAME "Info" header with a TOC | the TOC (as first built) | -186 / +5 ms (1:41 -91, 2:01 +279) |
| the same, after the fix below | | `CBR, Info header` | -29 / -47 ms (1:41 -42, 2:01 -51, 3:20 -46) |
| Air, "Le voyage de Penelope" | CBR 192, no header, a 351 KB ID3 tag (skipped) | the first frame's bitrate | -29 / -47 ms |
| Daft Punk, "Harder, Better, Faster, Stronger" | LAME VBR, Xing TOC | Xing TOC | -291 / +345 ms (0.13-0.15% of 3:44) |
| Kanye West, "Stronger" | FLAC with a SEEKTABLE | libFLAC's seek, 88-92 ms | 0 / 0 ms |
| Emancipator, "Awakenings" (7:00) | FLAC, no SEEKTABLE | libFLAC's seek, 75-112 ms | 0 / 0 ms (6:40: 0 ms, 112 ms) |

- All inside the targets (FLAC and CBR within 0.5 s, VBR with a TOC
  within 1-2%). A CBR MP3 lands 30-50 ms behind: the frame after the byte,
  and libmad drops the first frame (no bit reservoir). The card has no VBR
  file without a TOC and no FLAC with an ID3 tag in front.
- **No audio from before the start:** in every run the first 10 ms block
  is the fade-in, and from the second or third block the level matches
  the file at the landed second.
- **Now Playing before play** (`qs<sec>` while paused, screenshots): 1:00
  of 3:46 with the bar at 26% (78 of 296 px); 2:30 of 3:46; 1:00 of 3:08
  (Penelope: the held track's read-rate estimate after 6 s; the file is
  3:10, which the backend's seek line says); 2:30 of 3:44; 1:00 of 5:11.
- The decode task's `stack_free` low-water mark stayed 13,712 B through
  20-odd seeks (an MP3 played from 0:00 reaches it before any seek); no
  underruns.
- Edge cases: `qs222` on a 3:46 track and `qs300` both log `in its last
  5 s or past its end: from 0:00`; `qs1800` on `tone:silence` counts from
  30:00 (paused at 30:04).

**The CPU speed's restart, end to end** (the scripted finger on the
Output tab's row and the dialog's Restart):

- 240 -> 160, paused at 101.0 s: the boot logs `[queue] resume point:
  1:41 into 31 (stopped: play starts there)`, `restarted for the CPU
  speed` and the toast; Now Playing shows 1:41 of 3:46, Stopped
  (screenshot); nothing played. Play: `starting 1:41 in`, landed at
  1:41.029.
- 160 -> 240, the Restart tapped while playing: `restarting (paused
  first)`, `resume point saved: 2:01 into 31`, the same boot, 2:01 on Now
  Playing, play picks up there (+279 ms by the TOC then; the fix below).
- A hard reset (`!reset`, like a power cut) while paused at 2:12.8: the
  boot applies 2:12, stopped. Playing, then `!reset`: `resume point saved:
  none`, 0:00 and `--:--`, stopped. After every boot nothing played.
- On a waiting 1:00: prev plays the same entry from 0:00 (as prev from
  Stopped does); next and `i<n>` (another entry) drop it (`resume point
  cleared`). The sleep timer's pause saved one (`31:55 into 52`), and the
  flash's reset after it brought it back at boot.

**The texts** (screenshots): the row at 240 "Smoothest lists, dancing /
240 MHz" and at 160 "Slower lists, saves a little / 160 MHz", inside the
row; "Restart at 160 MHz?" with its 3-line body, and "Restart at 240 MHz?"
with "The speed changes at a restart. The music pauses and picks up at the
same second.", nothing cut.

**The held screen** (`Iu1`; the log, no screenshots: the console's `X` is
input and ends the warning):

- Lit: `Is32` right after a console wake: the warning at 2 s, `[screen]
  held lit while the toast counts down`, no dim for its 30 s (the dim was
  due 19 s after the wake).
- Dim: a wake, then `Is32` 23 s later: `dim -> bright (held lit for a
  countdown toast)` at the warning, lit to its end.
- Keep on: a scripted tap on it 10 s into the warning: `[power] idle: kept
  on (input)`, `[screen] the toast is gone: the countdown again (dims in
  19 s)`, then dim 20 s and off 30 s after the tap: the normal timeout.
  (The console bytes that carry a scripted tap are input themselves and
  end the warning in the same pass, so this shows the ending, not the
  button's own hit; a finger on it is still to do.)
- Off: `Is70` after the screen went off: `turning off in 30 s (the screen
  is off and stays off: it may be night)`, no screen line through it.
- The sleep fade: `Ts10` 1 s after a wake: the fade from 10 s held the
  screen bright for its 30 s, then at the pause `bright -> off (the sleep
  timer)`. `Ts40`: the screen was off when the fade began and stayed off.
- On USB each power-off ends in `USB power at the last moment: staying
  on` and the countdown starts again, so with `Is32` (shorter than the
  screen's timeout) the next warning comes 2 s later and holds the screen
  again, over and over. A test length's artefact: a real length (10 min
  or more) outlasts the screen's timeout.
- After the run: `Iu0`, `I0`.

**Found and fixed:**

- **A LAME CBR file was placed by its TOC** (its "Info" header has one):
  up to +279 ms at 2:01 of "Kelly Watch The Stars", where TrackSeek.h
  promised a CBR file to the frame. An "Info" header whose first frames
  agree on their bitrate now places the byte by that bitrate
  (`Mp3Seek::CbrInfo`, logged `CBR, Info header`); a file whose frames
  differ still goes by its TOC. test_track_seek's `test_mp3_info_cbr`.
  Measured after: -29 to -51 ms (the table).
- **Every boot without a resume point logged an error**,
  `[E][Preferences.cpp:539] getBytesLength(): nvs_get_blob len fail:
  resume NOT_FOUND`: `QueueStore`'s `readResume()` asks `isKey()` first.
  Gone from the boot log since.
- `pio test -e native`: 656 of 656 pass. `pio run -e core2`: RAM 53,728 B
  (unchanged), flash 2,144,691 B (+96). Flashed; the temporary dump is out
  of the tree.

**Seen, not changed:**

- Each pause writes the resume point to NVS and the next play removes it:
  two writes a pause (`qs` saves one too); 45 over this run. NVS spreads
  its writes; at a listener's rate of pauses this is nothing.
- A screenshot (`X`) of an off screen returns what it last showed (here
  the queue from before the run's edits): `Ps1` first.

**Still to do (a hand, the headphones or other files needed):**

- The power key off while paused, and the idle power-off to its end (on
  USB it stays on at the last moment; `Iu1` doesn't change that read):
  the second after the boot. (The same saved point as the pause's, which
  the hard reset above applied.)
- A finger on Keep on; a tap from Off during the sleep fade (the pocket
  guard; the scripted finger bypasses the wake latch).
- Over the headphones: a restart while streaming; a 48 kHz file refused.
- A file changed on the card since its point was saved; a VBR MP3 without
  a TOC; a FLAC with an ID3 tag in front.

The device was left on a normal boot at the defaults (`Pcb0`: "cpu_mhz"
removed, `CPU 240 MHz from boot (the default)`), the queue as found (27
tracks, at 8, stopped, no resume point: a `!reset` while playing), and the
serial daemon running.

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
