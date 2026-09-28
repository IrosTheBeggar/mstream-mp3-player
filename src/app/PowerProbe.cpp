#include "app/PowerProbe.h"

#include <M5Unified.h>

#include "storage/LocalStorage.h"

namespace {
constexpr const char* kCsvName = "power.csv";
constexpr const char* kCsvHeader =
    "ms,kind,label,samples,in_ma,in_ma_min,in_ma_max,in_w,in_w_min,in_w_max,acin_v,acin_ma,vbus_v,vbus_ma,"
    "bat_ma,bat_ma_min,bat_ma_max,bat_w,bat_v,aps_v,temp_c,status,state";

// The AXP192's registers (datasheet v1.13, section 9).
constexpr uint8_t kRegStatus = 0x00;
constexpr uint8_t kRegInputAdc = 0x56;    // 10 bytes: ACIN V, ACIN I, VBUS V, VBUS I, temperature
constexpr uint8_t kRegBatteryAdc = 0x78;  // 8 bytes: battery V, charge I, discharge I, APS V
constexpr uint8_t kRegAdcEnable1 = 0x82;  // 7 bat V, 6 bat I, 5 ACIN V, 4 ACIN I, 3 VBUS V, 2 VBUS I, 1 APS V, 0 TS
constexpr uint8_t kRegAdcEnable2 = 0x83;  // 7 internal temperature
constexpr uint8_t kRegAdcRate = 0x84;     // 7-6: 25 / 50 / 100 / 200 Hz
constexpr uint8_t kRegCoulomb = 0xB0;     // 8 bytes: charge count, discharge count (32 bits each, MSB first)
constexpr uint8_t kRegCoulombCtl = 0xB8;  // 7 enable, 6 pause, 5 clear
constexpr uint8_t kAdcWanted1 = 0xFE;     // everything but the TS pin
constexpr uint8_t kAdcWanted2 = 0x80;

uint32_t be32(const uint8_t* b) {
  return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
         (static_cast<uint32_t>(b[2]) << 8) | b[3];
}

// "ACIN" / "VBUS" / "ACIN+VBUS" / "none": the supplies reg 0x00 says are there.
const char* supplies(uint8_t status) {
  const bool acin = status & 0x80, vbus = status & 0x20;
  return acin && vbus ? "ACIN+VBUS" : acin ? "ACIN" : vbus ? "VBUS" : "none";
}
}  // namespace

void PowerProbe::ensureAdcs() {
  if (adcsChecked_) return;
  adcsChecked_ = true;
  auto& axp = M5.Power.Axp192;
  if (M5.Power.getType() != m5::Power_Class::pmic_axp192) {
    Serial.println("[power] no AXP192 on this board: nothing to measure");
    return;
  }
  const uint8_t e1 = axp.readRegister8(kRegAdcEnable1);
  const uint8_t e2 = axp.readRegister8(kRegAdcEnable2);
  const uint8_t rate = axp.readRegister8(kRegAdcRate);
  if ((e1 & kAdcWanted1) != kAdcWanted1) axp.writeRegister8(kRegAdcEnable1, e1 | kAdcWanted1);
  if ((e2 & kAdcWanted2) != kAdcWanted2) axp.writeRegister8(kRegAdcEnable2, e2 | kAdcWanted2);
  Serial.printf("[power] AXP192 ADCs: reg 0x82 0x%02x%s, 0x83 0x%02x%s, %lu Hz: battery V (1.1 mV), battery "
                "charge and discharge I (0.5 mA), ACIN and VBUS V (1.7 mV), ACIN I (0.625 mA), VBUS I "
                "(0.375 mA), APS V (1.4 mV), temperature (0.1 C)\n",
                e1 | kAdcWanted1, (e1 & kAdcWanted1) == kAdcWanted1 ? " (already on)" : " (switched on)",
                e2 | kAdcWanted2, (e2 & kAdcWanted2) == kAdcWanted2 ? " (already on)" : " (switched on)",
                (unsigned long)power::adcHz(rate));
}

bool PowerProbe::read(power::Sample* out) {
  if (M5.Power.getType() != m5::Power_Class::pmic_axp192) return false;
  auto& axp = M5.Power.Axp192;
  uint8_t status = 0;
  uint8_t in[10];
  uint8_t bat[8];
  if (!axp.readRegister(kRegStatus, &status, 1) || !axp.readRegister(kRegInputAdc, in, sizeof(in)) ||
      !axp.readRegister(kRegBatteryAdc, bat, sizeof(bat))) {
    ++readFailures_;
    return false;
  }
  *out = power::decode(status, in, bat);
  return true;
}

bool PowerProbe::sampleNow(power::Sample* out) {
  ensureAdcs();
  return read(out);
}

void PowerProbe::startWindow(uint32_t nowMs) {
  window_.reset(nowMs);
  nextSampleMs_ = nowMs;
}

void PowerProbe::loop(uint32_t nowMs) {
  if (!sampling_) return;
  if (static_cast<int32_t>(nowMs - nextSampleMs_) >= 0) {
    // On schedule (10 Hz), not "100 ms after the last": a slow pass doesn't
    // thin the window out.
    nextSampleMs_ = nowMs - nextSampleMs_ < 2 * kSampleMs ? nextSampleMs_ + kSampleMs : nowMs + kSampleMs;
    power::Sample s;
    if (read(&s)) window_.add(s);
  }
  // (A window started by a console command began at a later millis() than
  // this pass's `now`: not over, not 2^32 ms old.)
  if (!window_.over(nowMs, kWindowMs)) return;
  emit(nowMs, false);
  oneShot_ = false;
  if (!wanted()) {
    sampling_ = false;
    return;
  }
  window_.reset(nowMs);
}

void PowerProbe::emit(uint32_t nowMs, bool partial) {
  const uint32_t n = window_.count();
  const float secs = window_.elapsedMs(nowMs) / 1000.0f;
  char state[256] = "";
  if (describe_) describe_(state, sizeof(state));
  if (n == 0) {
    Serial.printf("[power] %.1f s: no samples (the power chip didn't answer %lu times) | %s\n", secs,
                  (unsigned long)readFailures_, state);
    return;
  }
  const power::Stat inMa = window_.inMa(), inW = window_.inW(), bat = window_.batMa();
  if (log_ || oneShot_ || partial) {
    char cc[96] = "";
    coulombText(cc, sizeof(cc), nowMs);
    // ACIN glitches (a single sample of 0-15 mA on USB, at 160/80 MHz):
    // left out of the numbers, counted here.
    char gl[24] = "";
    if (window_.glitches()) snprintf(gl, sizeof(gl), " glitches=%lu", (unsigned long)window_.glitches());
    // "in": what comes in from USB (ACIN and VBUS together; whichever the
    // cable is on reads it). "bat": + charging, - discharging.
    Serial.printf("[power] %.1f s%s n=%lu in=%.1f mA (%.1f..%.1f) %.3f W (%.3f..%.3f) [%s: ACIN %.2f V %.1f mA, "
                  "VBUS %.2f V %.1f mA] bat=%+.1f mA (%+.1f..%+.1f) %+.3f W %.3f V aps=%.2f V %.1f C%s%s | %s\n",
                  secs, partial ? " (partial)" : "", (unsigned long)n, inMa.mean, inMa.min, inMa.max, inW.mean,
                  inW.min, inW.max, supplies(window_.status()), window_.acinV(), window_.acinMa(), window_.vbusV(),
                  window_.vbusMa(), bat.mean, bat.min, bat.max, window_.batW(), window_.batV(), window_.apsV(),
                  window_.tempC(), gl, cc, state);
  }
  if (csv_) {
    char row[512];
    snprintf(row, sizeof(row),
             "%lu,%s,,%lu,%.1f,%.1f,%.1f,%.4f,%.4f,%.4f,%.3f,%.1f,%.3f,%.1f,%.1f,%.1f,%.1f,%.4f,%.3f,%.3f,%.1f,"
             "0x%02x,\"%s\"",
             (unsigned long)nowMs, partial ? "partial" : "window", (unsigned long)n, inMa.mean, inMa.min, inMa.max,
             inW.mean, inW.min, inW.max, window_.acinV(), window_.acinMa(), window_.vbusV(), window_.vbusMa(),
             bat.mean, bat.min, bat.max, window_.batW(), window_.batV(), window_.apsV(), window_.tempC(),
             window_.status(), state);
    appendCsv(row);
  }
}

void PowerProbe::requestLine(uint32_t nowMs) {
  ensureAdcs();
  oneShot_ = true;
  if (!sampling_) {
    sampling_ = true;
    startWindow(nowMs);
    Serial.println("[power] measuring for 5 s...");
  }
}

void PowerProbe::toggleLog(uint32_t nowMs) {
  log_ = !log_;
  if (log_) {
    ensureAdcs();
    if (!sampling_) {
      sampling_ = true;
      startWindow(nowMs);
    }
  }
  Serial.printf("[power] log: %s\n", log_ ? "a line every 5 s (Pl: off)" : "off");
}

void PowerProbe::toggleCsv(uint32_t nowMs) {
  if (!csv_ && !storage_.onCard()) {
    Serial.println("[power] csv: no microSD card (the flash isn't written for this)");
    return;
  }
  csv_ = !csv_;
  if (csv_) {
    ensureAdcs();
    if (!sampling_) {
      sampling_ = true;
      startWindow(nowMs);
    }
  }
  Serial.printf("[power] csv: %s%s/%s\n", csv_ ? "appending every 5 s to " : "off: ", storage_.stateDir(), kCsvName);
}

void PowerProbe::mark(const char* name, uint32_t nowMs) {
  // What was measured up to here belongs to the state before the change.
  if (sampling_ && window_.count() > 0 && (log_ || csv_)) emit(nowMs, true);
  Serial.printf("[power] ===== mark \"%s\" at %.1f s =====\n", name, nowMs / 1000.0f);
  if (csv_) {
    char row[160];
    // Quotes in the name would break the CSV: dropped.
    char label[64];
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < sizeof(label); ++i) {
      if (name[i] != '"' && name[i] != ',') label[j++] = name[i];
    }
    label[j] = 0;
    snprintf(row, sizeof(row), "%lu,mark,\"%s\"", (unsigned long)nowMs, label);
    appendCsv(row);
  }
  if (sampling_) startWindow(nowMs);
}

void PowerProbe::appendCsv(const char* row) {
  if (!storage_.available()) return;
  char path[48];
  snprintf(path, sizeof(path), "%s/%s", storage_.stateDir(), kCsvName);
  fs::FS& fs = storage_.fs();
  if (!csvHeader_) {
    csvHeader_ = true;
    if (!fs.exists(path)) {
      File h = fs.open(path, FILE_WRITE);
      if (h) {
        h.println(kCsvHeader);
        h.close();
      }
    }
  }
  // Opened and closed each time: a run on battery can end at any moment.
  File f = fs.open(path, FILE_APPEND);
  if (!f) {
    Serial.printf("[power] csv: can't write %s\n", path);
    return;
  }
  f.println(row);
  f.close();
}

void PowerProbe::coulombText(char* buf, size_t size, uint32_t nowMs) {
  buf[0] = 0;
  if (M5.Power.getType() != m5::Power_Class::pmic_axp192) return;
  auto& axp = M5.Power.Axp192;
  const uint8_t ctl = axp.readRegister8(kRegCoulombCtl);
  if (!(ctl & 0x80)) return;  // not counting
  uint8_t b[8];
  if (!axp.readRegister(kRegCoulomb, b, sizeof(b))) return;
  const uint32_t hz = power::adcHz(axp.readRegister8(kRegAdcRate));
  const double in = power::coulombMah(be32(b), hz), out = power::coulombMah(be32(b + 4), hz);
  const double net = in - out;
  if (coulombSinceMs_) {
    const double hours = (nowMs - coulombSinceMs_) / 3600000.0;
    snprintf(buf, size, " cc=+%.2f/-%.2f mAh (net %+.2f, %+.1f mA over %.2f h)", in, out, net,
             hours > 0 ? net / hours : 0.0, hours);
  } else {
    snprintf(buf, size, " cc=+%.2f/-%.2f mAh (net %+.2f)", in, out, net);
  }
}

void PowerProbe::coulomb(const char* arg, uint32_t nowMs) {
  if (M5.Power.getType() != m5::Power_Class::pmic_axp192) {
    Serial.println("[power] coulomb counter: no AXP192");
    return;
  }
  auto& axp = M5.Power.Axp192;
  if (arg[0] == '1') {
    axp.writeRegister8(kRegCoulombCtl, 0xA0);  // enable + clear (the clear bit clears itself)
    coulombSinceMs_ = nowMs ? nowMs : 1;
    Serial.println("[power] coulomb counter: cleared and counting (the battery's charge and discharge, in the "
                   "chip; P lines show it; Pq reads it, Pq0 stops it)");
    return;
  }
  if (arg[0] == '0') {
    axp.writeRegister8(kRegCoulombCtl, 0x00);
    coulombSinceMs_ = 0;
    Serial.println("[power] coulomb counter: off");
    return;
  }
  char text[96];
  coulombText(text, sizeof(text), nowMs);
  Serial.printf("[power] coulomb counter:%s\n", text[0] ? text : " off (Pq1 clears and starts it)");
}
