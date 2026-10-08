// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/PowerLab.h"

#include <M5Unified.h>
#include <esp_bt.h>
#include <soc/rtc.h>

#include "PowerChoices.h"
#include "TrackCatalog.h"
#include "app/BoardPower.h"
#include "app/PowerSettings.h"
#include "app/ScreenControl.h"
#include "storage/LocalStorage.h"

namespace {
namespace pc = powerchoice;

// M5GFX's Core2 backlight: brightness b > 0 sets DCDC3 to step (b >> 3) + 72
// of 25 mV from 0.7 V (2.5-3.275 V); 0 switches DCDC3 off.
uint32_t dc3Mv(uint8_t b) { return b ? 700u + 25u * ((b >> 3) + 72u) : 0u; }

int dbm(int level) { return -12 + 3 * level; }  // esp_power_level_t on the ESP32: 0 = -12 dBm ... 7 = +9 dBm

bool flag(const char* a, bool* on) {
  if ((a[0] != '0' && a[0] != '1') || a[1]) return false;
  *on = a[0] == '1';
  return true;
}

uint8_t ledLevel() { return static_cast<uint8_t>(255 - M5.Power.Axp192.readRegister8(0x9A)); }  // PWM1 duty, 255 = off
}  // namespace

PowerLab::PowerLab(Core2AudioBackend& audio, PlaybackController& player, DanceMode& dance, LocalStorage& storage,
                   ScreenControl& screen, PowerSettings& settings, Hooks hooks)
    : audio_(audio),
      player_(player),
      dance_(dance),
      screen_(screen),
      settings_(settings),
      hooks_(std::move(hooks)),
      probe_(storage, [this](char* buf, size_t size) { describe(buf, size); }) {}

// ---- the console ----

void PowerLab::command(const char* a) {
  const uint32_t now = millis();
  const char c = a[0];
  const char* arg = c ? a + 1 : a;
  switch (c) {
    case 0: probe_.requestLine(now); return;
    case 'l': probe_.toggleLog(now); return;
    case 'w': probe_.toggleCsv(now); return;
    case 'm': probe_.mark(arg[0] ? arg : "(unnamed)", now); return;
    case 'q': probe_.coulomb(arg, now); return;
    case 'b': backlight(arg); return;
    case 's': {
      bool on;
      if (!arg[0]) {
        screen_.printStatus();  // Ps: the screen policy's state
        return;
      }
      if (!flag(arg, &on)) break;
      screen(on);
      return;
    }
    case 'c': cpu(arg); return;
    case 't': txPower(arg); return;
    case 'e': exten(arg); return;
    case 'g': led(arg); return;
    case 'i': imu(arg); return;
    case 'f': touch(arg); return;
    case 'a': amp(arg); return;
    case 'd': loopDelay(arg); return;
    case 'k': taps(arg); return;
    case 'r': reconnect(arg); return;
    case 'z': playSilence(); return;
    default: break;
  }
  help();
}

void PowerLab::help() const {
  Serial.println("[power] P line (5 s), Pl log every 5 s, Pw csv on the card, Pm<name> mark, Pq/Pq1/Pq0 coulomb "
                 "counter; knobs: Pb<0-255> backlight while bright (Pb0 the setting's), Ps0/Ps1 screen off/on (Ps its state), Pc<mhz> cpu now (160<->80), "
                 "Pcb<160|240|0> cpu from boot (saved; 0 the default), Pt<min>,<max> bt tx levels 0-7 (a test), Pe0/1 5V boost, Pg0/1 "
                 "green led, Pi0/1 imu, Pf touch controller (Pf0/1 Active/Monitor now), Pa0/1 speaker amp, Pd<ms> loop idle delay (0 auto), Pk0/1 dance tracker + "
                 "taps, Pr0/1 bt background search rest now / a burst again, Pz play tone:silence next");
}

// The state after each line: the knobs and what the device is doing.
void PowerLab::describe(char* buf, size_t size) {
  BtSink& bt = audio_.bluetooth();
  const bool onBt = audio_.output() == Core2AudioBackend::Output::Bluetooth;
  const uint8_t b = screen_.backlight();
  int n = snprintf(buf, size,
                   "bl=%u (DC3 %lu mV) screen=%s cpu=%lu MHz play=%s out=%s link=%s stream=%s amp=%s "
                   "exten=%s led=%u imu=%s touch=%s taps=%s dance=%s bg=%s radio=%d%% loop=",
                   (unsigned)b, (unsigned long)dc3Mv(b), screen_.levelName(),
                   (unsigned long)getCpuFrequencyMhz(), hooks_.playState ? hooks_.playState() : "?",
                   onBt ? "bt" : "speaker", btPhaseName(bt.link().phase), bt.streamState(),
                   SpeakerSink::ampOn() ? (audio_.speaker().ampHeld() ? "held" : "on") : "off",
                   M5.Power.getExtOutput() ? "on" : "off", (unsigned)ledLevel(),
                   board::imuSuspended() ? "suspended" : "on", touchpower::modeName(board::touchModeKnown()),
                   audio_.tapsOn() ? "on" : "off",
                   dance_.active() ? (dance_.tracking() ? "shown" : "shown-untracked") : "hidden",
                   bt.reconnectPhase(), bt.radioBusyPercent());
  if (n < 0 || static_cast<size_t>(n) >= size) return;
  if (loopDelayMs_) {
    n += snprintf(buf + n, size - n, "%lums", (unsigned long)loopDelayMs_);
  } else {
    n += snprintf(buf + n, size - n, "auto");
  }
  if (n < 0 || static_cast<size_t>(n) >= size) return;
  dropReplacedTx();
  if (txMin_ >= 0) {
    snprintf(buf + n, size - n, " tx=%+d..%+d dBm (Pt)", dbm(txMin_), dbm(txMax_));
  } else {
    char range[16];
    pc::btRangeText(settings_.btChoice(), range, sizeof(range));
    snprintf(buf + n, size - n, " tx=%s (%s)", range, pc::btLabel(settings_.btChoice()));
  }
}

// ---- the knobs ----

void PowerLab::backlight(const char* a) {
  const uint8_t before = screen_.backlight();
  if (!a[0]) {
    Serial.printf("[power] backlight %u (DC3 %lu mV), screen %s\n", (unsigned)before, (unsigned long)dc3Mv(before),
                  screen_.levelName());
    return;
  }
  const long v = atol(a);
  if (!isDigit(a[0]) || v < 0 || v > 255) {
    Serial.println("[power] Pb<0-255>: the backlight while the screen is bright, until restart (Pb0: the setting's)");
    return;
  }
  // The screen policy owns the backlight: this is its Bright level for now
  // (it still dims and goes off; "Screen off after: Never" on the Output
  // tab holds it for a long measurement).
  if (!screen_.overrideBacklight(static_cast<uint8_t>(v))) {
    Serial.printf("[power] backlight: the screen is %s (Ps1, or a touch, first)\n", screen_.levelName());
    return;
  }
  const uint8_t after = screen_.backlight();
  Serial.printf("[power] backlight %u -> %u (DC3 %lu -> %lu mV%s; until restart, Pb0 the setting's)\n",
                (unsigned)before, (unsigned)after, (unsigned long)dc3Mv(before), (unsigned long)dc3Mv(after),
                after ? "" : ": DCDC3 off");
}

// The screen policy's own Off and wake (app/ScreenControl): the UI draws
// nothing while it's off, and a touch wakes it and does nothing else (with
// no input after that, it goes off again 10 s later: the pocket guard).
void PowerLab::screen(bool on) {
  if (on == !screen_.off()) {
    Serial.printf("[power] screen already %s (%s)\n", on ? "on" : "off", screen_.levelName());
    if (on) screen_.consoleOn();  // (dim: bright again)
    return;
  }
  if (on) {
    screen_.consoleOn();
  } else {
    screen_.consoleOff();
  }
  Serial.printf("[power] screen %s asked (a [screen] line confirms)\n", on ? "on" : "off");
}

bool PowerLab::audioBusy() {
  const PlayState s = player_.state();
  return s == PlayState::Playing || s == PlayState::Waiting || audio_.isPlaying() || audio_.bluetooth().streaming();
}

void PowerLab::cpu(const char* a) {
  rtc_cpu_freq_config_t cur;
  rtc_clk_cpu_freq_get_config(&cur);
  if (a[0] == 'b') {
    const char* v = a + 1;
    const long mhz = atol(v);
    if (!isDigit(v[0]) || (mhz != 0 && !pc::validCpuMhz(static_cast<uint32_t>(mhz)))) {
      Serial.printf("[power] Pcb<mhz>: the clock from boot, 160 or 240 (0: the default, %u); 80 only at runtime "
                    "(Pc80): decoding needs more\n",
                    (unsigned)pc::kDefaultCpuMhz);
      return;
    }
    // The Output tab's CPU speed is the same value: its row follows ("240
    // MHz until a restart" while the clock isn't the saved one yet).
    const uint16_t before = PowerSettings::cpuStored();
    if (mhz == 0) {
      settings_.clearCpu();
    } else if (!settings_.saveCpu(static_cast<uint16_t>(mhz))) {
      Serial.println("[power] CPU from boot: couldn't save it");
      return;
    }
    Serial.printf("[power] CPU from boot: %u MHz%s -> %u MHz%s (saved; restart to apply)\n",
                  (unsigned)pc::cpuMhzFromStored(before), pc::validCpuMhz(before) ? "" : " (the default)",
                  (unsigned)settings_.cpuSaved(), mhz ? "" : " (the default)");
    return;
  }
  if (!a[0]) {
    Serial.printf("[power] CPU %lu MHz (PLL %lu MHz); from boot: %u MHz (%s)\n", (unsigned long)cur.freq_mhz,
                  (unsigned long)cur.source_freq_mhz, (unsigned)settings_.cpuSaved(),
                  pc::validCpuMhz(PowerSettings::cpuStored()) ? "saved" : "the default");
    return;
  }
  const long mhz = atol(a);
  if (!isDigit(a[0]) || (mhz != 80 && mhz != 160 && mhz != 240)) {
    Serial.println("[power] Pc<mhz>: 80, 160 or 240 (Pc alone: what it is)");
    return;
  }
  if (static_cast<uint32_t>(mhz) == cur.freq_mhz) {
    Serial.printf("[power] CPU already %ld MHz\n", mhz);
    return;
  }
  rtc_cpu_freq_config_t want;
  if (!rtc_clk_cpu_freq_mhz_to_config(static_cast<uint32_t>(mhz), &want)) return;
  if (want.source_freq_mhz != cur.source_freq_mhz) {
    // 240 MHz runs from the PLL at 480 MHz, 160 and 80 from it at 320: the
    // switch would stop and retune the BBPLL, which the Bluetooth radio
    // (on since boot, page scan included) runs from.
    Serial.printf("[power] CPU %lu -> %ld MHz refused: it retunes the PLL (%lu -> %lu MHz) that the Bluetooth radio "
                  "runs from, and Bluetooth is on from boot. Pcb%ld (or the Output tab's CPU speed) sets "
                  "it from the next boot instead (before Bluetooth starts); from 160, Pc80 and Pc160 switch at runtime\n",
                  (unsigned long)cur.freq_mhz, mhz, (unsigned long)cur.source_freq_mhz,
                  (unsigned long)want.source_freq_mhz, mhz == 80 ? 160L : mhz);
    return;
  }
  if (mhz == 80 && audioBusy()) {
    // Decoding alone takes 30-39% of a core at 240 MHz (MASCOT-POC,
    // UI-SPIKE): 90-117% at 80, before the UI, SBC and Bluedroid.
    Serial.println("[power] CPU 80 MHz refused while audio runs (playing, waiting or streaming): decoding alone would "
                   "need about a whole core (30-39% of one at 240 MHz). Pause (and let the stream suspend, ~3 s) "
                   "first; it goes back to 160 by itself when audio starts");
    return;
  }
  const uint32_t before = getCpuFrequencyMhz();
  if (!setCpuFrequencyMhz(static_cast<uint32_t>(mhz))) {
    Serial.printf("[power] CPU %lu -> %ld MHz: failed\n", (unsigned long)before, mhz);
    return;
  }
  cpuReturnMhz_ = mhz == 80 ? before : 0;
  Serial.printf("[power] CPU %lu -> %lu MHz (APB %lu MHz)%s\n", (unsigned long)before,
                (unsigned long)getCpuFrequencyMhz(), (unsigned long)(getApbFrequency() / 1000000),
                mhz == 80 ? ": back to 160 by itself when audio starts" : "");
}

void PowerLab::dropReplacedTx() {
  if (txMin_ < 0 || settings_.btChanges() == txChanges_) return;
  char range[16];
  pc::btRangeText(settings_.btChoice(), range, sizeof(range));
  Serial.printf("[power] bt tx power: Pt's %+d..%+d dBm test replaced by the Bluetooth power row: %s (%s)\n",
                dbm(txMin_), dbm(txMax_), pc::btLabel(settings_.btChoice()), range);
  txMin_ = txMax_ = -1;
}

void PowerLab::txPower(const char* a) {
  dropReplacedTx();
  esp_power_level_t lo, hi;
  const esp_err_t got = esp_bredr_tx_power_get(&lo, &hi);
  if (!a[0]) {
    if (got != ESP_OK) {
      Serial.printf("[power] bt tx power: can't read it (%s)\n", esp_err_to_name(got));
      return;
    }
    char range[16];
    pc::btRangeText(settings_.btChoice(), range, sizeof(range));
    Serial.printf("[power] bt tx power: levels %d..%d (%+d..%+d dBm)%s; the setting: %s (%s)\n", lo, hi, dbm(lo),
                  dbm(hi), txMin_ < 0 ? "" : " (Pt, a test until restart)", pc::btLabel(settings_.btChoice()), range);
    return;
  }
  int mn = -1, mx = -1;
  if (sscanf(a, "%d,%d", &mn, &mx) != 2 || mn < 0 || mx > 7 || mn > mx) {
    const pc::TxLevels l = pc::btLevels(settings_.btChoice());
    Serial.printf("[power] Pt<min>,<max>: BR/EDR TX power levels 0-7 (-12, -9, -6, -3, 0, +3, +6, +9 dBm), a test "
                  "until restart; the setting (Output tab: Bluetooth power) is %s, %u,%u\n",
                  pc::btLabel(settings_.btChoice()), (unsigned)l.min, (unsigned)l.max);
    return;
  }
  const esp_err_t err =
      esp_bredr_tx_power_set(static_cast<esp_power_level_t>(mn), static_cast<esp_power_level_t>(mx));
  if (err != ESP_OK) {
    Serial.printf("[power] bt tx power: refused (%s)\n", esp_err_to_name(err));
    return;
  }
  txMin_ = static_cast<int8_t>(mn);
  txMax_ = static_cast<int8_t>(mx);
  txChanges_ = settings_.btChanges();
  if (audio_.bluetooth().connected()) {
    // The controller reads back the new range at once (seen on the device,
    // batch 3), but that is its setting, not the level of the link that is
    // up, which keeps its own: the new range counts from the next connection.
    Serial.printf("[power] bt tx power: %+d..%+d -> %+d..%+d dBm asked: applies from the next connection (the link "
                  "that is up keeps its level)\n",
                  dbm(lo), dbm(hi), dbm(mn), dbm(mx));
    return;
  }
  esp_power_level_t nlo = lo, nhi = hi;
  esp_bredr_tx_power_get(&nlo, &nhi);
  Serial.printf("[power] bt tx power: %+d..%+d -> %+d..%+d dBm (from the next page, scan or connection)\n", dbm(lo),
                dbm(hi), dbm(nlo), dbm(nhi));
}

void PowerLab::exten(const char* a) {
  bool on;
  const bool before = M5.Power.getExtOutput();
  if (!flag(a, &on)) {
    Serial.printf("[power] 5 V boost (EXTEN): %s (Pe0 / Pe1)\n", before ? "on" : "off");
    return;
  }
  // On the Core2's AXP192 this also sets GPIO0 (the bus's VBUS switch):
  // M5Unified's setExtOutput(), as at boot.
  M5.Power.setExtOutput(on);
  Serial.printf("[power] 5 V boost (EXTEN, the M-Bus/Grove 5 V): %s -> %s\n", before ? "on" : "off",
                M5.Power.getExtOutput() ? "on" : "off");
}

void PowerLab::led(const char* a) {
  bool on;
  const uint8_t before = ledLevel();
  if (!flag(a, &on)) {
    Serial.printf("[power] green LED: %u/255 (Pg0 / Pg1)\n", (unsigned)before);
    return;
  }
  M5.Power.setLed(on ? 255 : 0);
  Serial.printf("[power] green LED: %u -> %u/255\n", (unsigned)before, (unsigned)ledLevel());
}

void PowerLab::imu(const char* a) {
  bool on;
  const int addr = board::bmi270Address();
  if (addr < 0) {
    Serial.println("[power] imu: no BMI270 found (only that one is handled)");
    return;
  }
  const uint8_t conf0 = board::bmi270PwrConf(addr);
  const uint8_t ctrl0 = board::bmi270PwrCtrl(addr);
  if (!flag(a, &on)) {
    Serial.printf("[power] imu (BMI270 at 0x%02x): PWR_CTRL 0x%02x PWR_CONF 0x%02x: %s (Pi0 / Pi1; suspended from "
                  "boot)\n",
                  addr, ctrl0, conf0, board::imuSuspended() ? "suspended" : "on");
    return;
  }
  board::setImuSuspended(addr, !on);
  Serial.printf("[power] imu (BMI270 at 0x%02x): PWR_CTRL 0x%02x -> 0x%02x, PWR_CONF 0x%02x -> 0x%02x (%s)\n", addr,
                ctrl0, board::bmi270PwrCtrl(addr), conf0, board::bmi270PwrConf(addr), on ? "on" : "suspended");
}

// The FT6336U touch controller's power mode (ENERGY.md section 5, P1). This
// chip goes back to Monitor by itself 30 s after the last touch (0x86 = 1,
// 0x87 = 30), so an A/B holds Active by repeating Pf0 (every 10 s) against
// Pf1, the screen off (Ps0), with Pl.
void PowerLab::touch(const char* a) {
  namespace tp = touchpower;
  switch (tp::parsePf(a)) {
    case tp::Pf::Report: {
      tp::Regs r;
      if (!board::readTouchPower(&r)) {
        Serial.println("[power] touch: no answer at 0x38");
        return;
      }
      char line[160];
      tp::describe(r, line, sizeof(line));
      Serial.printf("[power] touch: %s (Pf0 / Pf1 Active / Monitor now)\n", line);
      return;
    }
    case tp::Pf::Active:
    case tp::Pf::Monitor: {
      const uint8_t want = tp::parsePf(a) == tp::Pf::Active ? tp::kActive : tp::kMonitor;
      const int before = board::readTouchMode();
      if (!board::setTouchMode(want)) {
        Serial.println("[power] touch: no answer at 0x38");
        return;
      }
      const int after = board::readTouchMode();
      Serial.printf("[power] touch: %s -> %s (reads back %s)%s\n", tp::modeName(before), tp::modeName(want),
                    tp::modeName(after),
                    want == tp::kMonitor ? ": until a touch takes it back to Active"
                                         : ": until it goes back to Monitor by itself (Pf: after how long untouched)");
      return;
    }
    case tp::Pf::Refused: break;
  }
  Serial.println("[power] Pf: the touch controller's registers; Pf0 Active, Pf1 Monitor (its slow scan; a touch "
                 "takes it back to Active, and with ctrl=1 it returns to Monitor by itself once untouched). "
                 "Hibernate (3) is refused: only a reset brings the chip out of it, and its reset line is the LCD's");
}

void PowerLab::amp(const char* a) {
  bool on;
  const bool before = SpeakerSink::ampOn();
  if (!flag(a, &on)) {
    Serial.printf("[power] speaker amp: %s%s%s (Pa0 / Pa1; by itself it goes off 2 s after the speaker goes "
                  "quiet)\n",
                  before ? "on" : "off", audio_.speaker().ampHeld() ? ", held on by Pa1" : "",
                  audio_.speaker().ampPending() ? ", a switch pending" : "");
    return;
  }
  if (!on && audio_.output() == Core2AudioBackend::Output::Speaker && player_.state() == PlayState::Playing) {
    Serial.println("[power] speaker amp: the speaker is playing (pause first, or move the output)");
    return;
  }
  audio_.speaker().requestAmp(on ? SpeakerSink::Amp::On : SpeakerSink::Amp::Off);
  ampWatch_ = true;
  ampBefore_ = before;
  Serial.printf("[power] speaker amp: %s -> %s asked (once the speaker is quiet)%s\n", before ? "on" : "off",
                on ? "on" : "off",
                on ? ": the I2S clocks zeros into it, nothing is heard; held on until Pa0"
                   : " without the 2 s wait; the next speaker playback turns it on");
}

void PowerLab::loopDelay(const char* a) {
  const long ms = atol(a);
  if (!isDigit(a[0]) || ms < 0 || ms > 100) {
    Serial.printf("[power] Pd<ms>: the loop's idle delay while nothing animates, 1-100 (0: the UI's own, ~5 ms); "
                  "now %s\n",
                  loopDelayMs_ ? (String(loopDelayMs_) + " ms").c_str() : "the UI's own");
    return;
  }
  const uint32_t before = loopDelayMs_;
  loopDelayMs_ = static_cast<uint32_t>(ms);
  Serial.printf("[power] loop idle delay: %s -> %s (not while a list moves or the Dance tab is up; touches are read "
                "that much later)\n",
                before ? (String(before) + " ms").c_str() : "auto", ms ? (String(ms) + " ms").c_str() : "auto");
}

uint32_t PowerLab::loopDelayMs(uint32_t uiIdleMs, bool danceActive) const {
  // The UI asks for less than 5 ms only when a list frame is due soon.
  if (!loopDelayMs_ || danceActive || uiIdleMs < 5) return uiIdleMs;
  return loopDelayMs_;
}

void PowerLab::taps(const char* a) {
  bool on;
  if (!flag(a, &on)) {
    Serial.printf("[power] taps %s, dance tracker %s (Pk0 / Pk1)\n", audio_.tapsOn() ? "on" : "off",
                  dance_.tracking() ? "on" : "off");
    return;
  }
  const bool taps0 = audio_.tapsOn(), track0 = dance_.tracking();
  dance_.setTracking(on);  // the taps follow it: on only while the Dance tab is up and tracking
  Serial.printf("[power] the dance beat tracker %s -> %s, the outputs' taps %s -> %s (on only while the Dance tab "
                "is up)\n",
                track0 ? "on" : "off", on ? "on" : "off", taps0 ? "on" : "off", audio_.tapsOn() ? "on" : "off");
}

void PowerLab::reconnect(const char* a) {
  bool on;
  BtSink& bt = audio_.bluetooth();
  if (!flag(a, &on)) {
    Serial.printf("[power] bt background search: %s, radio busy %d%% of the last minute (Pr0 rest now / Pr1 a burst "
                  "again)\n",
                  bt.reconnectPhase(), bt.radioBusyPercent());
    return;
  }
  bt.setBackgroundReconnect(on);
  Serial.printf("[power] bt background search: %s -> %s asked (a [bt] line confirms; a connect or the Pair screen "
                "starts it again)\n",
                bt.reconnectPhase(), on ? "a burst" : "resting");
}

void PowerLab::playSilence() {
  const uint32_t id = TrackCatalog().find(TrackCatalog::kSilencePath);
  if (id == TrackCatalog::kNone) {
    Serial.println("[power] silence: no such track");
    return;
  }
  const char* silent = hooks_.silentMode && hooks_.silentMode() ? "; silent test mode" : "";
  // A full queue (the queue's cap, docs/QUEUE-MODES.md 15: a card of 5,000
  // tracks or more after its first boot, a Play all or a Shuffle all)
  // refuses an add: the silence plays as the queue then, and qu puts the
  // listener's back (the Play's undo).
  if (player_.queue().room() == 0) {
    if (!player_.playNow(&id, 1, 0)) {
      Serial.println("[power] silence: couldn't play it (no memory)");
      return;
    }
    Serial.printf("[power] playing %s as the queue (the queue was full, %lu: qu puts it back; an hour of zeros: the "
                  "output runs at its full rate, nothing is heard%s)\n",
                  TrackCatalog::kSilencePath, static_cast<unsigned long>(QueueModel::kMaxEntries), silent);
    return;
  }
  const int at = player_.currentIndex() + 1;  // 0 with an empty queue
  if (!player_.playNext(&id, 1)) {
    Serial.println("[power] silence: couldn't queue it (no memory)");
    return;
  }
  player_.play(static_cast<size_t>(at));
  Serial.printf("[power] playing %s (queue entry %d, an hour of zeros: the output runs at its full rate, nothing is "
                "heard%s)\n",
                TrackCatalog::kSilencePath, at, silent);
}

void PowerLab::loop(uint32_t nowMs) {
  dropReplacedTx();  // (its line right after the row's tap, not at the next P)
  probe_.loop(nowMs);
  // Pc80 is for quiet spells: audio needs the clock back.
  if (cpuReturnMhz_ && getCpuFrequencyMhz() == 80 && audioBusy()) {
    const uint32_t to = cpuReturnMhz_;
    cpuReturnMhz_ = 0;
    setCpuFrequencyMhz(to);
    Serial.printf("[power] CPU 80 -> %lu MHz: audio started\n", (unsigned long)getCpuFrequencyMhz());
  }
  if (ampWatch_ && !audio_.speaker().ampPending()) {
    ampWatch_ = false;
    Serial.printf("[power] speaker amp: %s -> %s (M5.Speaker %s)\n", ampBefore_ ? "on" : "off",
                  SpeakerSink::ampOn() ? "on" : "off", SpeakerSink::ampOn() ? "running" : "ended: I2S stopped, AXP192 GPIO2 low");
  }
}
