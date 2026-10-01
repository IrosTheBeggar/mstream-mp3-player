// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/SerialConsole.h"

#include <Arduino.h>

void SerialConsole::begin() {
  const uint32_t t0 = millis();
  while (Serial.available() == 0 && millis() - t0 < HostLine::kQuietMs) delay(1);
  char c = 0;
  if (Serial.available() == 0) (void)host_.quiet(millis(), &c);  // (nothing read: nothing held)
}

bool SerialConsole::poll() {
  bool any = false;
  const uint32_t now = millis();
  // Input lost? The IDF driver only drops bytes once the receive buffer is
  // full (up to a FIFO's 128 B short: its last chunk didn't fit), and only
  // this reads it, so a buffer this full as a poll starts may have lost
  // some. What follows a loss may be the middle of a line: Sync again.
  const int waiting = Serial.available();
  if (waiting >= static_cast<int>(kRxBuffer) - 128) {
    if (pending_ != Pending::None) Serial.printf("> %c: abandoned (input was lost)\n", pendingKey_);
    pending_ = Pending::None;
    arg_ = "";
    host_.resync(now);
    syncLogged_ = false;
    Serial.printf("[console] %d bytes waiting: input may have been lost; a line under way is dropped\n", waiting);
  }
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    any = true;
    // A computer's '@' line first: none of its bytes is a key. ('@' is text
    // only inside an R argument that has some: "Rttone:1000@48000".)
    const HostLine::Byte kind = host_.push(c, pending_ == Pending::Rate && arg_.length() > 0, now);
    switch (kind) {
      case HostLine::Byte::Console:
        key(c);
        break;
      case HostLine::Byte::Start:
        if (pending_ != Pending::None) {
          Serial.printf("> %c: abandoned (a computer's line began)\n", pendingKey_);
          pending_ = Pending::None;
          arg_ = "";
        }
        break;
      case HostLine::Byte::Line:
      case HostLine::Byte::Bad:
      case HostLine::Byte::Long:
      case HostLine::Byte::Restart:
        if (actions_.hostLine) actions_.hostLine(kind == HostLine::Byte::Line ? host_.text() : nullptr, kind);
        break;
      case HostLine::Byte::Dropped:
        syncLogged_ = false;
        break;
      case HostLine::Byte::Taken:
      case HostLine::Byte::Held:
        break;
    }
  }
  // Nothing more for now. Sync (a boot, lost input) ends after a quiet
  // moment, and a byte it held was a keypress.
  if (host_.syncing()) {
    char c = 0;
    if (host_.quiet(millis(), &c)) key(c);
    if (!host_.syncing()) {
      if (!syncLogged_ && host_.dropped() > 0) {
        Serial.printf("[console] dropped %lu bytes of a computer's line already under way (a boot or lost input; "
                      "a key sent with its Enter just then went too: send it again)\n",
                      static_cast<unsigned long>(host_.dropped()));
      }
      syncLogged_ = true;
    }
  }
  return any;
}

void SerialConsole::key(char c) {
  if (pending_ != Pending::None) {
    if (c != '\n' && c != '\r') {  // collect the argument up to Enter
      arg_ += c;
      return;
    }
    const Pending what = pending_;
    pending_ = Pending::None;
    arg_.trim();
    switch (what) {
      case Pending::PlayIndex:
        Serial.printf("> play %ld\n", arg_.toInt());
        actions_.playIndex(static_cast<int>(arg_.toInt()));
        break;
      case Pending::Bench:
        Serial.printf("> bench %ld\n", arg_.toInt());
        actions_.bench(static_cast<int>(arg_.toInt()));
        break;
      case Pending::HeadphonesName:
        Serial.printf("> headphones name \"%s\"\n", arg_.c_str());
        actions_.setHeadphonesName(arg_.c_str());
        break;
      case Pending::Headroom: {
        const long db = arg_.toInt();
        if (arg_.length() == 0 || !isDigit(arg_[0]) || db > 12) {
          Serial.println("> headroom: h<n> with n 0-12 (dB)");
          break;
        }
        Serial.printf("> bluetooth headroom -%ld dB\n", db);
        actions_.setHeadroom(static_cast<int>(db));
        break;
      }
      case Pending::TempoPrior: {
        const float bpm = arg_.toFloat();  // "" -> 0: clear
        if (bpm != 0.0f && (bpm < 30.0f || bpm > 300.0f)) {
          Serial.println("> tempo prior: t<bpm> with bpm 30-300, or t / t0 to clear");
          break;
        }
        Serial.printf("> tempo prior %.1f\n", bpm);
        actions_.tempoPrior(bpm);
        break;
      }
      case Pending::DanceOffset:
        Serial.printf("> dance latency offset %+ld ms\n", arg_.toInt());
        actions_.danceOffset(static_cast<int>(arg_.toInt()));
        break;
      case Pending::Freeze: {
        if (arg_.length() == 0) {
          Serial.println("> unfreeze");
          actions_.freezePose(-1);
          break;
        }
        const long n = arg_.toInt();
        if (!isDigit(arg_[0]) || n > 15) {
          Serial.println("> freeze: k<n> with n 0-15, or k alone to unfreeze");
          break;
        }
        Serial.printf("> freeze %ld\n", n);
        actions_.freezePose(static_cast<int>(n));
        break;
      }
      case Pending::InputLab:
        Serial.printf("> input lab \"%s\"\n", arg_.c_str());
        actions_.inputLab(arg_.c_str());
        break;
      case Pending::ScrollLab:
        Serial.printf("> scroll lab \"%s\"\n", arg_.c_str());
        actions_.scrollLab(arg_.c_str());
        break;
      case Pending::LibraryIndex:
        Serial.printf("> library index \"%s\"\n", arg_.c_str());
        actions_.libraryIndex(arg_.c_str());
        break;
      case Pending::FontProbe:
        Serial.printf("> font probe \"%s\"\n", arg_.c_str());
        actions_.fontProbe(arg_.c_str());
        break;
      case Pending::ThumbProbe:
        Serial.printf("> thumbnail probe \"%s\"\n", arg_.c_str());
        actions_.thumbProbe(arg_.c_str());
        break;
      case Pending::Queue:
        Serial.printf("> queue \"%s\"\n", arg_.c_str());
        actions_.queue(arg_.c_str());
        break;
      case Pending::Touch:
        Serial.printf("> input \"%s\"\n", arg_.c_str());
        actions_.touch(arg_.c_str());
        break;
      case Pending::Power:
        Serial.printf("> power \"%s\"\n", arg_.c_str());
        if (actions_.power) actions_.power(arg_.c_str());
        break;
      case Pending::Sleep:
        Serial.printf("> sleep timer \"%s\"\n", arg_.c_str());
        if (actions_.sleep) actions_.sleep(arg_.c_str());
        break;
      case Pending::Idle:
        Serial.printf("> idle power-off \"%s\"\n", arg_.c_str());
        if (actions_.idle) actions_.idle(arg_.c_str());
        break;
      case Pending::BluetoothTest:
        Serial.printf("> bluetooth test \"%s\"\n", arg_.c_str());
        if (actions_.bluetoothTest) actions_.bluetoothTest(arg_.c_str());
        break;
      case Pending::Rate:
        Serial.printf("> rate converter \"%s\"\n", arg_.c_str());
        if (actions_.rate) actions_.rate(arg_.c_str());
        break;
      case Pending::None:
        break;
    }
    return;
  }
  switch (c) {
    case 'n': Serial.println("> next"); actions_.next(); break;
    case 'p': Serial.println("> prev"); actions_.prev(); break;
    case ' ': Serial.println("> play/pause"); actions_.playPause(); break;
    case 'o': Serial.println("> output"); actions_.toggleOutput(); break;
    case '+': actions_.stepVolume(+10); break;
    case '-': actions_.stepVolume(-10); break;
    case 's': actions_.printStats(); break;
    case 'l': actions_.listTracks(); break;
    case 'f': Serial.println("> forget bluetooth device"); actions_.forgetBluetooth(); break;
    case 'i': pending_ = Pending::PlayIndex; arg_ = ""; break;
    case 'b': pending_ = Pending::Bench; arg_ = ""; break;
    case 'c': pending_ = Pending::HeadphonesName; arg_ = ""; break;
    case 'h': pending_ = Pending::Headroom; arg_ = ""; break;
    case 'z': Serial.println("> silent test mode"); actions_.silentMode(); break;
    case 'd': Serial.println("> dance screen"); actions_.toggleDance(); break;
    case 'm': Serial.println("> next dancer"); actions_.cycleSkin(); break;
    case 'x': Serial.println("> screenshot of the dancer"); actions_.screenshot(false); break;
    case 'X': Serial.println("> screenshot of the screen"); actions_.screenshot(true); break;
    case 'v': actions_.toggleBeatLog(); break;
    case 'L': if (actions_.partitionTable) actions_.partitionTable(); break;
    case 't': pending_ = Pending::TempoPrior; arg_ = ""; break;
    case 'y': pending_ = Pending::DanceOffset; arg_ = ""; break;
    case 'k': pending_ = Pending::Freeze; arg_ = ""; break;
    case 'u': pending_ = Pending::InputLab; arg_ = ""; break;
    case 'w': pending_ = Pending::ScrollLab; arg_ = ""; break;
    case 'g': pending_ = Pending::LibraryIndex; arg_ = ""; break;
    case 'e': pending_ = Pending::FontProbe; arg_ = ""; break;
    case 'j': pending_ = Pending::ThumbProbe; arg_ = ""; break;
    case 'q': pending_ = Pending::Queue; arg_ = ""; break;
    case 'a': pending_ = Pending::Touch; arg_ = ""; break;
    case 'P': pending_ = Pending::Power; arg_ = ""; break;
    case 'T': pending_ = Pending::Sleep; arg_ = ""; break;
    case 'I': pending_ = Pending::Idle; arg_ = ""; break;
    case 'B': pending_ = Pending::BluetoothTest; arg_ = ""; break;
    case 'R': pending_ = Pending::Rate; arg_ = ""; break;
    default: break;  // newlines etc.
  }
  if (pending_ != Pending::None) pendingKey_ = c;
}
