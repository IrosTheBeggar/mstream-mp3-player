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
      case 'h': pending_ = Pending::Headroom; arg_ = ""; break;
      case 'z': Serial.println("> silent test mode"); actions_.silentMode(); break;
      case 'd': Serial.println("> dance screen"); actions_.toggleDance(); break;
      case 'm': Serial.println("> next dancer"); actions_.cycleSkin(); break;
      case 'x': Serial.println("> screenshot of the dancer"); actions_.screenshot(false); break;
      case 'X': Serial.println("> screenshot of the screen"); actions_.screenshot(true); break;
      case 'v': actions_.toggleBeatLog(); break;
      case 't': pending_ = Pending::TempoPrior; arg_ = ""; break;
      case 'y': pending_ = Pending::DanceOffset; arg_ = ""; break;
      case 'k': pending_ = Pending::Freeze; arg_ = ""; break;
      case 'u': pending_ = Pending::InputLab; arg_ = ""; break;
      case 'w': pending_ = Pending::ScrollLab; arg_ = ""; break;
      case 'g': pending_ = Pending::LibraryIndex; arg_ = ""; break;
      case 'e': pending_ = Pending::FontProbe; arg_ = ""; break;
      case 'j': pending_ = Pending::ThumbProbe; arg_ = ""; break;
      default: break;  // newlines etc.
    }
  }
}
