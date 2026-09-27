#include "app/PowerLab.h"

#include <M5Unified.h>
#include <Preferences.h>
#include <esp_bt.h>
#include <soc/rtc.h>

#include "TrackCatalog.h"
#include "storage/LocalStorage.h"
#include "ui/LcdLock.h"

namespace {
constexpr const char* kPrefs = "power";
constexpr const char* kPrefsBootMhz = "cpu_mhz";

// What applyBootClock() found and did, for logBootClock().
uint16_t bootSavedMhz = 0;
uint32_t bootFromMhz = 0;
bool bootApplied = false;

// BMI270 (datasheet rev 1.x): chip id 0x24 at reg 0x00; PWR_CONF 0x7C
// (bit 0 advanced power save), PWR_CTRL 0x7D (bit 3 temperature, 2 accel,
// 1 gyro, 0 aux). Suspend: everything off, then power save on (~3.5 uA).
constexpr uint8_t kBmiChipId = 0x24;
constexpr uint8_t kBmiRegChipId = 0x00;
constexpr uint8_t kBmiPwrConf = 0x7C;
constexpr uint8_t kBmiPwrCtrl = 0x7D;
constexpr uint8_t kBmiOnCtrl = 0x0E;  // as M5Unified leaves it: temperature, accel, gyro
constexpr uint32_t kI2cHz = 400000;

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
                   Hooks hooks)
    : audio_(audio),
      player_(player),
      dance_(dance),
      hooks_(std::move(hooks)),
      probe_(storage, [this](char* buf, size_t size) { describe(buf, size); }) {}

// ---- the boot clock (Pcb) ----

void PowerLab::applyBootClock() {
  Preferences p;
  if (!p.begin(kPrefs, false)) return;
  bootSavedMhz = p.getUShort(kPrefsBootMhz, 0);
  p.end();
  bootFromMhz = getCpuFrequencyMhz();
  // Before Bluetooth starts: 240 <-> 160 retunes the PLL the radio runs
  // from, which is only safe while the radio is off.
  if (bootSavedMhz == 160 && bootFromMhz != 160) bootApplied = setCpuFrequencyMhz(160);
}

void PowerLab::logBootClock() {
  if (!bootSavedMhz) return;
  Serial.printf("[power] CPU %lu MHz from boot (saved by Pcb%u%s; Pcb0 goes back to the default)\n",
                (unsigned long)getCpuFrequencyMhz(), (unsigned)bootSavedMhz,
                bootApplied ? "" : bootFromMhz == bootSavedMhz ? ", already" : ": COULDN'T SET IT");
}

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
      if (!flag(arg, &on)) break;
      screen(on, "console");
      return;
    }
    case 'c': cpu(arg); return;
    case 't': txPower(arg); return;
    case 'e': exten(arg); return;
    case 'g': led(arg); return;
    case 'i': imu(arg); return;
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
                 "counter; knobs: Pb<0-255> backlight, Ps0/Ps1 screen off/on, Pc<mhz> cpu now (160<->80), "
                 "Pcb<160|240|0> cpu from boot (saved), Pt<min>,<max> bt tx levels 0-7, Pe0/1 5V boost, Pg0/1 "
                 "green led, Pi0/1 imu, Pa0/1 speaker amp, Pd<ms> loop idle delay (0 auto), Pk0/1 dance tracker + "
                 "taps, Pr0/1 bt background reconnect, Pz play tone:silence next");
}

// The state after each line: the knobs and what the device is doing.
void PowerLab::describe(char* buf, size_t size) {
  BtSink& bt = audio_.bluetooth();
  const bool onBt = audio_.output() == Core2AudioBackend::Output::Bluetooth;
  const uint8_t b = M5.Display.getBrightness();
  int n = snprintf(buf, size,
                   "bl=%u (DC3 %lu mV) screen=%s cpu=%lu MHz play=%s out=%s link=%s stream=%s amp=%s "
                   "exten=%s led=%u imu=%s taps=%s dance=%s bg=%s loop=",
                   (unsigned)b, (unsigned long)(screenOff_ ? 0 : dc3Mv(b)), screenOff_ ? "off" : "on",
                   (unsigned long)getCpuFrequencyMhz(), hooks_.playState ? hooks_.playState() : "?",
                   onBt ? "bt" : "speaker", btPhaseName(bt.link().phase), bt.streamState(),
                   SpeakerSink::ampOn() ? "on" : "off", M5.Power.getExtOutput() ? "on" : "off", (unsigned)ledLevel(),
                   imuSuspended_ ? "suspended" : "on", audio_.tapsOn() ? "on" : "off",
                   dance_.active() ? (dance_.tracking() ? "shown" : "shown-untracked") : "hidden",
                   bt.backgroundReconnectPaused() ? "paused" : "on");
  if (n < 0 || static_cast<size_t>(n) >= size) return;
  if (loopDelayMs_) {
    n += snprintf(buf + n, size - n, "%lums", (unsigned long)loopDelayMs_);
  } else {
    n += snprintf(buf + n, size - n, "auto");
  }
  if (n < 0 || static_cast<size_t>(n) >= size) return;
  if (txMin_ >= 0) snprintf(buf + n, size - n, " tx=%+d..%+d dBm", dbm(txMin_), dbm(txMax_));
}

// ---- the knobs ----

void PowerLab::backlight(const char* a) {
  const uint8_t before = M5.Display.getBrightness();
  if (!a[0]) {
    Serial.printf("[power] backlight %u (DC3 %lu mV)%s\n", (unsigned)before, (unsigned long)dc3Mv(before),
                  screenOff_ ? ", screen off" : "");
    return;
  }
  const long v = atol(a);
  if (!isDigit(a[0]) || v < 0 || v > 255) {
    Serial.println("[power] Pb<0-255>: the backlight");
    return;
  }
  if (screenOff_) {
    Serial.println("[power] backlight: the screen is off (Ps1 first)");
    return;
  }
  M5.Display.setBrightness(static_cast<uint8_t>(v));
  const uint8_t after = M5.Display.getBrightness();
  Serial.printf("[power] backlight %u -> %u (DC3 %lu -> %lu mV%s)\n", (unsigned)before, (unsigned)after,
                (unsigned long)dc3Mv(before), (unsigned long)dc3Mv(after), after ? "" : ": DCDC3 off");
}

void PowerLab::screen(bool on, const char* why) {
  if (on != screenOff_) {
    Serial.printf("[power] screen already %s\n", on ? "on" : "off");
    return;
  }
  {
    LcdLock lock;  // the LCD shares the SPI bus with the card
    if (on) {
      M5.Display.wakeup();  // sleep-out, then the backlight back at its level
    } else {
      M5.Display.sleep();  // backlight off (DCDC3), then sleep-in (the panel keeps its RAM)
    }
  }
  screenOff_ = !on;
  Serial.printf("[power] screen %s -> %s (%s; backlight %u)%s\n", on ? "off" : "on", on ? "on" : "off", why,
                (unsigned)M5.Display.getBrightness(),
                on ? "" : ": the UI still draws into the panel's RAM; the first touch wakes it and does nothing else");
}

bool PowerLab::holdInput() {
  if (!screenOff_ && !swallow_) return false;
  const bool touched = M5.Touch.getCount() > 0;
  if (screenOff_) {
    if (!touched) return true;
    screen(true, "woken by a touch, which is swallowed");
    swallow_ = true;
    quietPasses_ = 0;
    return true;
  }
  // The finger that woke it: nothing it does counts, its lift included
  // (a button clicks on the lift), so two passes without a touch first.
  quietPasses_ = touched ? 0 : quietPasses_ + 1;
  if (quietPasses_ >= 2) swallow_ = false;
  return true;
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
    if (!isDigit(v[0]) || (mhz != 0 && mhz != 160 && mhz != 240)) {
      Serial.println("[power] Pcb<mhz>: the clock from boot, 160 or 240 (0: the default, 240); 80 only at runtime "
                     "(Pc80): decoding needs more");
      return;
    }
    Preferences p;
    if (!p.begin(kPrefs, false)) return;
    const uint16_t before = p.getUShort(kPrefsBootMhz, 0);
    if (mhz == 0 || mhz == 240) {
      p.remove(kPrefsBootMhz);
    } else {
      p.putUShort(kPrefsBootMhz, static_cast<uint16_t>(mhz));
    }
    p.end();
    Serial.printf("[power] CPU from boot: %s -> %s (saved; restart to apply)\n", before ? String(before).c_str() : "default",
                  mhz == 160 ? "160" : "default");
    return;
  }
  if (!a[0]) {
    Preferences p;
    uint16_t boot = 0;
    if (p.begin(kPrefs, false)) {
      boot = p.getUShort(kPrefsBootMhz, 0);
      p.end();
    }
    Serial.printf("[power] CPU %lu MHz (PLL %lu MHz); from boot: %s\n", (unsigned long)cur.freq_mhz,
                  (unsigned long)cur.source_freq_mhz, boot ? String(boot).c_str() : "default (240, Arduino's F_CPU)");
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
                  "runs from, and Bluetooth is on from boot. Pcb%ld sets it from the next boot instead (before "
                  "Bluetooth starts); from 160, Pc80 and Pc160 switch at runtime\n",
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

void PowerLab::txPower(const char* a) {
  esp_power_level_t lo, hi;
  const esp_err_t got = esp_bredr_tx_power_get(&lo, &hi);
  if (!a[0]) {
    if (got != ESP_OK) {
      Serial.printf("[power] bt tx power: can't read it (%s)\n", esp_err_to_name(got));
      return;
    }
    Serial.printf("[power] bt tx power: levels %d..%d (%+d..%+d dBm)%s\n", lo, hi, dbm(lo), dbm(hi),
                  txMin_ < 0 ? " (the default)" : "");
    return;
  }
  int mn = -1, mx = -1;
  if (sscanf(a, "%d,%d", &mn, &mx) != 2 || mn < 0 || mx > 7 || mn > mx) {
    Serial.println("[power] Pt<min>,<max>: BR/EDR TX power levels 0-7 (-12, -9, -6, -3, 0, +3, +6, +9 dBm); the "
                   "default is 4,5 (0..+3 dBm)");
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
  esp_power_level_t nlo = lo, nhi = hi;
  esp_bredr_tx_power_get(&nlo, &nhi);
  Serial.printf("[power] bt tx power: %+d..%+d -> %+d..%+d dBm (from the next page, scan or connection: a link that "
                "is up may keep its level until it reconnects)\n",
                dbm(lo), dbm(hi), dbm(nlo), dbm(nhi));
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

int PowerLab::bmi270Address() {
  if (M5.Imu.getType() != m5::imu_t::imu_bmi270) return -1;
  for (uint8_t addr : {0x68, 0x69}) {
    if (M5.In_I2C.readRegister8(addr, kBmiRegChipId, kI2cHz) == kBmiChipId) return addr;
  }
  return -1;
}

void PowerLab::imu(const char* a) {
  bool on;
  const int addr = bmi270Address();
  if (addr < 0) {
    Serial.println("[power] imu: no BMI270 found (only that one is handled)");
    return;
  }
  const uint8_t conf0 = M5.In_I2C.readRegister8(addr, kBmiPwrConf, kI2cHz);
  const uint8_t ctrl0 = M5.In_I2C.readRegister8(addr, kBmiPwrCtrl, kI2cHz);
  if (!flag(a, &on)) {
    Serial.printf("[power] imu (BMI270 at 0x%02x): PWR_CTRL 0x%02x PWR_CONF 0x%02x: %s (Pi0 / Pi1)\n", addr, ctrl0,
                  conf0, imuSuspended_ ? "suspended" : "on");
    return;
  }
  if (on) {
    M5.In_I2C.writeRegister8(addr, kBmiPwrConf, 0x00, kI2cHz);  // power save off first
    delayMicroseconds(1000);                                     // (450 us before the next write)
    M5.In_I2C.writeRegister8(addr, kBmiPwrCtrl, kBmiOnCtrl, kI2cHz);
  } else {
    M5.In_I2C.writeRegister8(addr, kBmiPwrCtrl, 0x00, kI2cHz);  // accel, gyro, temperature off
    delayMicroseconds(1000);
    M5.In_I2C.writeRegister8(addr, kBmiPwrConf, 0x01, kI2cHz);  // advanced power save: suspend
  }
  delayMicroseconds(1000);
  imuSuspended_ = !on;
  Serial.printf("[power] imu (BMI270 at 0x%02x): PWR_CTRL 0x%02x -> 0x%02x, PWR_CONF 0x%02x -> 0x%02x (%s)\n", addr,
                ctrl0, M5.In_I2C.readRegister8(addr, kBmiPwrCtrl, kI2cHz), conf0,
                M5.In_I2C.readRegister8(addr, kBmiPwrConf, kI2cHz), on ? "on" : "suspended");
}

void PowerLab::amp(const char* a) {
  bool on;
  const bool before = SpeakerSink::ampOn();
  if (!flag(a, &on)) {
    Serial.printf("[power] speaker amp: %s%s (Pa0 / Pa1)\n", before ? "on" : "off",
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
                on ? ": the I2S clocks zeros into it, nothing is heard" : "; the next speaker playback turns it on");
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
  audio_.setTapsOn(on);
  dance_.setTracking(on);
  Serial.printf("[power] the outputs' taps %s -> %s, the dance beat tracker %s -> %s\n", taps0 ? "on" : "off",
                on ? "on" : "off", track0 ? "on" : "off", on ? "on" : "off");
}

void PowerLab::reconnect(const char* a) {
  bool on;
  BtSink& bt = audio_.bluetooth();
  const bool paused = bt.backgroundReconnectPaused();
  if (!flag(a, &on)) {
    Serial.printf("[power] bt background reconnect: %s (Pr0 / Pr1)\n", paused ? "paused" : "on");
    return;
  }
  bt.setBackgroundReconnect(on);
  Serial.printf("[power] bt background reconnect: %s -> %s asked (a [bt] line confirms; a connect or the Pair "
                "screen resumes it)\n",
                paused ? "paused" : "on", on ? "on" : "paused");
}

void PowerLab::playSilence() {
  const uint32_t id = TrackCatalog().find(TrackCatalog::kSilencePath);
  const int at = player_.currentIndex() + 1;  // 0 with an empty queue
  if (id == TrackCatalog::kNone || !player_.playNext(&id, 1)) {
    Serial.println("[power] silence: couldn't queue it");
    return;
  }
  player_.play(static_cast<size_t>(at));
  Serial.printf("[power] playing %s (queue entry %d, an hour of zeros: the output runs at its full rate, nothing is "
                "heard%s)\n",
                TrackCatalog::kSilencePath, at, hooks_.silentMode && hooks_.silentMode() ? "; silent test mode" : "");
}

void PowerLab::loop(uint32_t nowMs) {
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
