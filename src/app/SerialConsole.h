#pragma once
#include <Arduino.h>

#include <functional>
#include <utility>

// Single-key commands over USB serial, so tests can be scripted from the host:
//   n next   p prev   <space> play/pause   o switch output   + / - volume
//   s stats  l list tracks   f forget the remembered Bluetooth device and restart
// and commands that take an argument, ended with Enter:
//   i<n> play track n (0-based)   b<n> benchmark decoding track n
//   c<name> connect to headphones whose name contains <name> (saved)
//   h<n> Bluetooth headroom -n dB, 0-12 (a diagnostic, not saved; default 2)
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
  };

  explicit SerialConsole(Actions actions) : actions_(std::move(actions)) {}

  // Handles whatever has arrived; call from the main loop.
  void poll();

private:
  enum class Pending { None, PlayIndex, Bench, HeadphonesName, Headroom };

  Actions actions_;
  Pending pending_ = Pending::None;  // a command waiting for its argument
  String arg_;
};
