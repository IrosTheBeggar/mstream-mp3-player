#include "app/SerialConsole.h"

#include <Arduino.h>

void SerialConsole::poll() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (pending_ != Pending::None) {
      if (c != '\n' && c != '\r') {  // collect the argument up to Enter
        arg_ += c;
        continue;
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
        case Pending::None:
          break;
      }
      continue;
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
      default: break;  // newlines etc.
    }
  }
}
