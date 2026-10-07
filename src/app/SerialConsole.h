// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include <functional>
#include <utility>

#include "HostLine.h"

// Single-key commands over USB serial, so tests can be scripted from the host:
//   n next   p prev   <space> play/pause   o switch output   + / - volume
//   s stats  l list tracks   f forget the remembered Bluetooth device and restart
//   z silent test mode (speaker at volume 0, Bluetooth doesn't take over; until restart)
//   d dance screen on/off   m next dancer (crab, stick)
//   x / X screenshot of the dancer's box / the whole screen
//   v per-beat log on/off
//   L the partition table as flashed, the running app slot and its version, NVS use (bug reports)
// and commands that take an argument, ended with Enter:
//   i<n> play track n (0-based)   b<n> benchmark decoding track n, b</path> a file by its path
//   O... Opus (docs/OPUS.md): O status, Ol/Oi/Oh the decoder's state in the pinned block /
//        internal RAM / PSRAM above 0x3FA00000 from the next open (M0's A/B), Ot1/Ot0 the
//        converter's table copy in the pinned PSRAM block (the default) / internal RAM (M2's G5 check)
//   c<name> the name a build with BT_SINK_NAME scans for, with none remembered (saved)
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
//   B... Bluetooth tests that leave the listener's pairing alone: B status;
//        Bs auto-pair by signal for the next scan (RAM only, off at boot, logged;
//        starts that scan: none may be remembered, so Bn first), Bs0 off; Bf the next boot as a
//        fresh unit (NVS flag cleared by that boot: as if nothing were remembered
//        and there were no BT_SINK_NAME, the stored address and bond untouched;
//        restarts now); Bn the same for this session (RAM only, not while linked
//        or pairing), Bn0 back
//   R... the rate converter (docs/RESAMPLER.md): R status, Rt the test tracks, Rt<n> or
//        Rt<tone:...> play one on its own (the player stopped: nothing follows it;
//        silence only on Bluetooth, a tone only in silent mode), Rf</music/...> a file
//        the same way (silent mode only), Rx stops either, Rb its bench
//   G... gapless playback (docs/GAPLESS.md): G status, G0/G1 off/on, Gt0/Gt1
//        trimming by the LAME tag off/on, Gx<n> the ring's cut against a reader
//        on the other core (stops the player)
// '@' lines are a computer's, not commands (docs/USB-VISUALIZER.md: the USB
// visualizer's protocol; HostLine): every byte from an '@' to the end of
// its line goes to hostLine, never to the keys above, and is never echoed.
// An '@' abandons a command still waiting for its argument (logged), except
// inside an R argument that has text ("Rttone:1000@48000"). Typed by hand
// such a line gets an "@err" reply and does nothing else. Reading that
// starts mid-line (a boot while a computer sends, input lost to an
// overflow) drops the line's tail instead of running it as keys (HostLine's
// Sync, logged): the first key after a boot, sent with its Enter, may need
// sending again.
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
    // The bench: the argument as typed (a queue entry's number, or a path).
    std::function<void(const char*)> bench;
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
    // Bluetooth tests (B): the argument as typed (may be "").
    std::function<void(const char*)> bluetoothTest;
    // The partition table as flashed (L).
    std::function<void()> partitionTable;
    // The rate converter (R): the argument as typed (may be "").
    std::function<void(const char*)> rate;
    // Gapless playback (G): the argument as typed (may be "").
    std::function<void(const char*)> gapless;
    // Opus (O): the argument as typed (may be "").
    std::function<void(const char*)> opus;
    // A computer's '@' line: complete (HostLine::Byte::Line: `line` is its
    // text from the '@', writable) or not one (Bad, Long, Restart: `line`
    // nullptr).
    std::function<void(char* line, HostLine::Byte kind)> hostLine;
  };

  // The receive buffer setup() gives Serial before it starts (the
  // visualizer's lines must outlast a slow loop pass: docs/USB-VISUALIZER.md).
  static constexpr size_t kRxBuffer = 1024;

  explicit SerialConsole(Actions actions) : actions_(std::move(actions)) {}

  // Right after Serial starts (setup(), before the banner): waits up to
  // HostLine::kQuietMs for input. None: no computer's line was under way
  // as Serial started, and keys are keys from the first byte.
  void begin();
  // Handles whatever has arrived; call from the main loop. True: something
  // arrived (someone is at the console: the idle power-off waits).
  bool poll();

private:
  void key(char c);  // a byte that is the console's
  enum class Pending {
    None, PlayIndex, Bench, HeadphonesName, Headroom, TempoPrior, DanceOffset, Freeze,
    InputLab, ScrollLab, LibraryIndex, FontProbe, ThumbProbe, Queue, Touch, Power, Sleep, Idle, BluetoothTest,
    Rate, Gapless, Opus,
  };

  Actions actions_;
  Pending pending_ = Pending::None;  // a command waiting for its argument
  char pendingKey_ = 0;              // ... its letter (for the log when it is abandoned)
  String arg_;
  HostLine host_;                    // a computer's '@' line being read (256 B)
  bool syncLogged_ = true;           // Sync's end logged (or nothing to log)
};
