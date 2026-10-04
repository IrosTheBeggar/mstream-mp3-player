// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// What the Pair screen's search found, for the serial log. A pairing that
// took three tries (2026-10-04) left nobody able to tell whether the
// failed searches saw the headphones and missed them, or saw nothing:
// the results only filled the list the screen shows, and the non-audio
// ones were dropped without a trace.
//
// The Bluetooth (BTC) task never prints: it posts every result of the
// search (audio or not) into a PairFindRing, under BtSink's scan lock,
// and the loop tells them apart here and logs:
//   - one line per audio device the first time this search sees it (its
//     name or "(no name)", RSSI, class of device, whether it is the
//     remembered headphones or the ones linked now);
//   - one more line when a name arrives later for one logged without
//     (mostly as a result of its own: below);
//   - nothing for the rest (repeats: an inquiry round answers every ~10 s;
//     other devices: only counted);
//   - when the search ends, a summary: how many devices it saw, how many
//     of them audio and which, how many results, and any lost.
// Portable, no allocation: BtSink keeps one of each in PSRAM.

// One result of the search, as the BTC task saw it. Two kinds:
//   - an inquiry result: the class of device, the RSSI, and the name if
//     the device put it in its EIR (an audio device's: noteDiscovery());
//   - a name alone (cod 0, no RSSI): a device that answered the inquiry
//     without its name in the EIR is asked for it once the inquiry round
//     ends (Bluedroid's remote name request), and the answer comes as a
//     result of its own, with the name and nothing else. It says nothing about the device's class:
//     PairFinds takes it for the device an inquiry result already showed.
struct PairFind {
  uint8_t addr[6] = {};
  char name[32] = "";       // from the result or its EIR (audio devices); "" none
  int8_t rssi = -127;       // -127: none in the result
  uint32_t cod = 0;         // class of device (0: none in the result: a name alone)
  bool audio = false;       // an audio device (the Pair list's rule)
  bool remembered = false;  // the headphones remembered now
  bool linked = false;      // linked now (left out of the list)
  uint16_t search = 0;      // the search it belongs to (BtSink's count)
};

// The BTC task's results on their way to the loop: a fixed ring, FIFO.
// Not thread-safe by itself: BtSink holds its spinlock around every call
// (each is short: one copy). Full, a result is dropped and counted (the
// loop drains it every pass, so that means the loop stalled).
class PairFindRing {
public:
  static constexpr int kSize = 16;
  bool push(const PairFind& f);
  bool pop(PairFind& out);
  int size() const { return n_; }
  uint32_t dropped() const { return dropped_; }
  // Empty, and the dropped count back to 0 (a search starts).
  void clear() {
    head_ = n_ = 0;
    dropped_ = 0;
  }

private:
  PairFind d_[kSize];
  int head_ = 0;  // the oldest
  int n_ = 0;
  uint32_t dropped_ = 0;
};

// One search's finds, on the loop: told apart by address (the first
// kDevices; results from devices past those are only counted, so a
// crowded room can't make the log repeat itself).
class PairFinds {
public:
  static constexpr int kDevices = 48;
  // What a result needs said.
  enum class Say : uint8_t {
    Nothing,  // seen before, not audio, or a name that isn't news: counted at most
    Found,    // an audio device this search hadn't seen
    Named,    // one logged without a name: its name came
  };

  // A search starts (everything forgotten).
  void start(uint32_t nowMs);
  // A result of this search. A name alone (cod 0) names a device an
  // inquiry result of this search showed (Named if it is audio and was
  // logged without one; a non-audio one keeps it quietly); for any other
  // address it says nothing and isn't counted (its inquiry result came
  // before the search, or the ring lost it: counted there).
  Say note(const PairFind& f);
  // The device a Found or Named result was about, as recorded: its name
  // (from whichever result had it), the class and RSSI of the result that
  // found it, remembered or linked if any result said so. What describe()
  // prints (a name alone has no class or RSSI of its own).
  const PairFind& told() const { return seen_[told_]; }

  int devices() const { return n_; }          // told apart
  int audioDevices() const { return audio_; }
  uint32_t results() const { return results_; }  // inquiry results (names alone aren't)
  uint32_t untold() const { return untold_; }  // results from devices past kDevices (untoldAudio() of them audio)
  uint32_t untoldAudio() const { return untoldAudio_; }

  // The line for a Found or Named result (`f`: told() after it), without
  // "[bt] pair: " or the address (the firmware adds both; addresses stay
  // out of the tests):
  //   found "SPYDRONE" (headphones, class 0x240404), rssi -62, the remembered headphones
  //   found (no name) (speaker, class 0x240414), rssi -81
  //   the one found with no name is "JBL Flip 5" (speaker, class 0x240414), rssi -81
  static void describe(const PairFind& f, Say say, char* buf, size_t size);
  // The search's summary, at its end; `lost`: results the ring dropped.
  //   the search saw 9 devices in 81 s, 2 of them audio: "SPYDRONE"
  //   (remembered), (no name); 41 inquiry results
  //   the search saw nothing in 120 s (not one inquiry result)
  // The names are cut with ", ..." where the buffer runs out.
  void summary(uint32_t nowMs, uint32_t lost, char* buf, size_t size) const;

private:
  // Each device told apart, as recorded (told(); `search` unused).
  PairFind seen_[kDevices];
  int n_ = 0;
  int told_ = 0;  // seen_'s index of the last Found or Named
  int audio_ = 0;
  uint32_t results_ = 0;
  uint32_t untold_ = 0;
  uint32_t untoldAudio_ = 0;
  uint32_t startMs_ = 0;
};
