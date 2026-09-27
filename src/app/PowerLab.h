#pragma once
#include <Arduino.h>

#include <functional>

#include "PlaybackController.h"
#include "app/DanceMode.h"
#include "app/PowerProbe.h"
#include "audio/Core2AudioBackend.h"

class LocalStorage;

// The console's P commands (with Enter): measuring what the Core2 draws
// (PowerProbe) and A/B knobs that switch one consumer at a time, for
// finding what each one costs. Every knob says what it was and what it is
// now, is undone by its opposite, and nothing of it is saved except the
// boot clock (Pcb). All off by default: until a P command, this costs a
// few comparisons per loop pass.
//
//   P          one [power] line at the end of a 5 s window
//   Pl         a [power] line every 5 s, on / off
//   Pw         the lines to /.player/power.csv on the card, on / off (battery runs)
//   Pm<name>   a marker (the window ends there)
//   Pq / Pq1 / Pq0   the coulomb counter: read / clear and start / stop
//   Pb<0-255>  the backlight (M5GFX brightness: AXP192 DCDC3, 0 = off)
//   Ps0 / Ps1  the screen off (backlight off, ILI9342C sleep-in) / on; while
//              it's off the first touch wakes it and does nothing else
//   Pc<mhz>    the CPU clock now: 160 <-> 80 (same PLL; 80 not while audio runs)
//   Pcb<mhz>   the CPU clock from boot, saved: 160 or 240 (Pcb0: the default)
//   Pt<min>,<max>  Bluetooth BR/EDR TX power levels 0-7 (-12..+9 dBm, 3 dB steps)
//   Pe0 / Pe1  the 5 V boost (EXTEN, the M-Bus/Grove 5 V) off / on
//   Pg0 / Pg1  the green LED off / on
//   Pi0 / Pi1  the IMU (BMI270) suspended / on
//   Pa0 / Pa1  the speaker amp (and M5.Speaker's I2S) off / on (zeros: silent)
//   Pd<ms>     the loop's idle delay when nothing animates, 1-100 (Pd0: the UI's own)
//   Pk0 / Pk1  the dance beat tracker and the outputs' taps off / on
//   Pr0 / Pr1  the background Bluetooth reconnect paused / resumed
//   Pz         play "tone:silence" next (an hour of zeros: full-rate output, silent)
class PowerLab {
public:
  struct Hooks {
    std::function<const char*()> playState;  // "playing", "paused", ...
    std::function<bool()> silentMode;        // the console's z test mode
  };

  PowerLab(Core2AudioBackend& audio, PlaybackController& player, DanceMode& dance, LocalStorage& storage,
           Hooks hooks);

  // setup(), before M5.begin() and Bluetooth: the boot clock saved by Pcb.
  static void applyBootClock();
  // setup(), once Serial is up: says what applyBootClock() did.
  static void logBootClock();

  // The console: the text after 'P', as typed.
  void command(const char* arg);
  // Loop task, every pass.
  void loop(uint32_t nowMs);
  // Before the input layer reads the touch, every pass: true while the
  // screen is off (and until the finger that woke it lifts): the input is
  // suspended, so the waking touch does nothing else.
  bool holdInput();
  // What the loop sleeps: the UI's own wait, or the Pd knob's while
  // nothing animates (no list frame due, the Dance tab not up).
  uint32_t loopDelayMs(uint32_t uiIdleMs, bool danceActive) const;

private:
  void help() const;
  void describe(char* buf, size_t size);
  void backlight(const char* a);
  void screen(bool on, const char* why);
  void cpu(const char* a);
  void txPower(const char* a);
  void exten(const char* a);
  void led(const char* a);
  void imu(const char* a);
  void amp(const char* a);
  void loopDelay(const char* a);
  void taps(const char* a);
  void reconnect(const char* a);
  void playSilence();
  bool audioBusy();
  int bmi270Address();

  Core2AudioBackend& audio_;
  PlaybackController& player_;
  DanceMode& dance_;
  Hooks hooks_;
  PowerProbe probe_;

  bool screenOff_ = false;
  bool swallow_ = false;      // the touch that woke the screen is still down
  uint8_t quietPasses_ = 0;   // passes without a touch since
  uint32_t loopDelayMs_ = 0;  // 0: the UI's own
  bool imuSuspended_ = false;
  int8_t txMin_ = -1, txMax_ = -1;  // set by Pt, -1: the default
  uint32_t cpuReturnMhz_ = 0;       // Pc80: the clock to go back to once audio runs
  bool ampWatch_ = false;           // a Pa request to report once done
  bool ampBefore_ = false;
};
