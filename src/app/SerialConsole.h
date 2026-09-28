#pragma once
#include <Arduino.h>

#include <functional>
#include <utility>

// Single-key commands over USB serial, so tests can be scripted from the host:
//   n next   p prev   <space> play/pause   o switch output   + / - volume
//   s stats  l list tracks   f forget the remembered Bluetooth device and restart
//   z silent test mode (speaker at volume 0, Bluetooth doesn't take over; until restart)
//   d dance screen on/off   m next dancer (crab, stick)
//   x / X screenshot of the dancer's box / the whole screen
//   v per-beat log on/off
// and commands that take an argument, ended with Enter:
//   i<n> play track n (0-based)   b<n> benchmark decoding track n
//   c<name> connect to headphones whose name contains <name> (saved)
//   h<n> Bluetooth headroom -n dB, 0-12 (a diagnostic, not saved; default 2)
//   t<bpm> tempo prior for the beat tracker (t or t0 clears it; a track change does too)
//   y<ms> dance latency offset, + later / - earlier (not saved)
//   k<n> freeze the dancer at phase n/8 of a two-beat cycle, 0-15 (k alone: follow the beat)
//   q... the queue: q status, qa play all, qb built-ins, ql albums, qp<n>/qn<n>/q+<n> album n:
//        play / play next / add, qr<pos> remove, qc clear up next, qx clear, qu undo
//   a... the input layer: a touch calibration (a5-a9 with fewer crosshairs), ac check the
//        touch, as status, ad the default table, ah0/ah1 haptics, ar0/ar1 rail ticks, aq close
//        (a, not "cal": c<name> is the headphones' name)
//   P... power measurements (app/PowerLab): P a [power] line, Pl log, Pw csv, Pm<name> mark,
//        Pq coulomb counter, and A/B knobs (P? lists them)
//   T... the sleep timer (docs/ENERGY.md section 3): T status, T<min> minutes from now,
//        Ts<sec> seconds (tests), Tt / Ta / Tq the end of the track / album / queue,
//        T+ +10 min, T0 off
//   I... the idle power-off (docs/ENERGY.md item 4): I status, I<min> a test length in
//        minutes, Is<sec> in seconds (until restart), I0 the setting's again; tests:
//        Iu1/Iu0 pretend on battery (the last-moment USB read stays real), Ib<sec>
//        the next boot's toast note
// and the UI spike's tools (docs/UI-SPIKE.md), also ended with Enter, the
// text after the letter passed on as it is:
//   u...  input lab (u toggles; u0-u3 modes; us summary)
//   w...  scroll lab (w toggles; w0 interactive, w1-w3 stress)
//   g...  library index (g report, g0 from the SD card, g<n> synthetic)
//   e...  font probe (e all, e1-e5 one option)
//   j...  thumbnail probe (j first cover, j<n>, jw<n> with .565 files, ja all)
// (No 'f' for fonts: f forgets the headphones and restarts.)
class SerialConsole {
public:
  struct Actions {
    std::function<void()> next;
    std::function<void()> prev;
    std::function<void()> playPause;
    std::function<void()> toggleOutput;
    std::function<void(int)> stepVolume;
    std::function<void()> printStats;
    std::function<void()> listTracks;
    std::function<void(int)> playIndex;
    std::function<void(int)> bench;
    std::function<void()> forgetBluetooth;
    std::function<void(const char*)> setHeadphonesName;
    std::function<void(int)> setHeadroom;
    std::function<void()> silentMode;
    std::function<void()> toggleDance;
    std::function<void()> cycleSkin;
    std::function<void(bool)> screenshot;  // true: the whole screen
    std::function<void()> toggleBeatLog;
    std::function<void(float)> tempoPrior;
    std::function<void(int)> danceOffset;
    std::function<void(int)> freezePose;   // -1: unfreeze
    // UI spike: the argument as typed (may be "").
    std::function<void(const char*)> inputLab;
    std::function<void(const char*)> scrollLab;
    std::function<void(const char*)> libraryIndex;
    std::function<void(const char*)> fontProbe;
    std::function<void(const char*)> thumbProbe;
    // The queue: the argument as typed (may be "").
    std::function<void(const char*)> queue;
    // The input layer (touch calibration, haptics): the argument as typed.
    std::function<void(const char*)> touch;
    // Power measurements and knobs (app/PowerLab): the argument as typed.
    std::function<void(const char*)> power;
    // The sleep timer: the argument as typed (may be "").
    std::function<void(const char*)> sleep;
    // The idle power-off: the argument as typed (may be "").
    std::function<void(const char*)> idle;
  };

  explicit SerialConsole(Actions actions) : actions_(std::move(actions)) {}

  // Handles whatever has arrived; call from the main loop. True: something
  // arrived (someone is at the console: the idle power-off waits).
  bool poll();

private:
  enum class Pending {
    None, PlayIndex, Bench, HeadphonesName, Headroom, TempoPrior, DanceOffset, Freeze,
    InputLab, ScrollLab, LibraryIndex, FontProbe, ThumbProbe, Queue, Touch, Power, Sleep, Idle,
  };

  Actions actions_;
  Pending pending_ = Pending::None;  // a command waiting for its argument
  String arg_;
};
