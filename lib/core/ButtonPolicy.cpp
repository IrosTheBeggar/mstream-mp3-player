#include "ButtonPolicy.h"

ButtonGesture::Config ButtonPolicy::gestureFor(int button) {
  ButtonGesture::Config c;
  c.holdMs = kHoldMs;
  c.repeatMs = button == kButtonB ? 0 : kVolumeRepeatMs;
  return c;
}

void ButtonPolicy::note(Feedback f, uint32_t ms) {
  f.ms = ms;
  f.seq = feedback_.seq + 1;
  feedback_ = f;
}

bool ButtonPolicy::handle(const InputEvent& e, Transport& t) {
  using T = InputEvent::Type;
  if (!e.isButton()) return false;
  const int b = e.button;
  if (e.type == T::Click) {
    if (b == kButtonA) {
      t.prev();
    } else if (b == kButtonB) {
      t.playPause();
    } else if (b == kButtonC) {
      t.next();
    } else {
      return false;
    }
    return true;
  }
  if (e.type != T::Hold && e.type != T::Repeat) return false;
  if (b == kButtonA || b == kButtonC) {
    t.stepVolume(b == kButtonA ? -kVolumeStep : kVolumeStep);
    Feedback f;
    f.kind = Hud::Volume;
    f.volume = t.volume();
    note(f, e.ms);
    return true;
  }
  if (b != kButtonB || e.type != T::Hold) return false;
  Feedback f;
  f.kind = Hud::Output;
  f.toBluetooth = !t.onBluetooth();
  if (!f.toBluetooth && t.playing()) {
    t.pause();  // never on to the speaker still playing
    f.paused = true;
  }
  f.refused = !t.switchOutput();
  note(f, e.ms);
  return true;
}
