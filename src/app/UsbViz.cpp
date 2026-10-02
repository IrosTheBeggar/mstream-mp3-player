// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/UsbViz.h"

#include <esp_timer.h>

void UsbViz::onLine(char* line, HostLine::Byte kind) {
  // Stamped as soon as the console hands it over: the heard clock follows
  // the least delayed samples, so the loop's lateness only adds spread.
  const auto nowUs = static_cast<uint32_t>(esp_timer_get_time());
  const uint32_t nowMs = millis();
  if (kind != HostLine::Byte::Line || !line) {
    apply(link_.bad(kind, nowMs), nowUs);
    return;
  }
  const HostLink::Busy busy = link_.active() || !hooks_.busy ? HostLink::Busy::None : hooks_.busy();
  const HostLink::Out o = link_.line(line, nowMs, busy);
  // A refused @hello, once per reason (the sender retries every second).
  if (o.event == HostLink::Event::None && busy != HostLink::Busy::None && busy != lastRefusal_ &&
      strncmp(o.reply, "@err 4 ", 7) == 0) {
    lastRefusal_ = busy;
    Serial.printf("[viz] a computer asked to drive the dancer: not now (%s)\n", HostLink::busyName(busy));
  }
  apply(o, nowUs);
}

void UsbViz::loop(uint32_t nowMs, bool usbPower) {
  const auto nowUs = static_cast<uint32_t>(esp_timer_get_time());
  apply(link_.poll(nowMs, usbPower), nowUs);
  // (Defensive: every way off the tab the listener has is an exit already:
  // a tab tap is a touch outside the dancer.)
  if (link_.active() && hooks_.danceGone && hooks_.danceGone()) {
    apply(link_.end(HostLink::Why::DanceGone, nowMs), nowUs);
  }
}

void UsbViz::userEnded(HostLink::Why why) {
  apply(link_.end(why, millis()), static_cast<uint32_t>(esp_timer_get_time()));
}

void UsbViz::apply(const HostLink::Out& o, uint32_t nowUs) {
  switch (o.event) {
    case HostLink::Event::Enter: {
      lastRefusal_ = HostLink::Busy::None;
      dance_.setHost(true, &link_.stats());
      const bool paused = hooks_.enter ? hooks_.enter() : false;
      Serial.printf("%s\n", o.reply);
      // (Quiet: a burst of pages under way, as after a boot, finishes its
      // tries first, then the search rests instead of backing off.)
      Serial.printf("[viz] on: dancing to the computer (protocol %lu, session %s); %s; the headphones' search "
                    "quiet\n",
                    static_cast<unsigned long>(link_.proto()), link_.session(),
                    paused ? "the player paused" : "nothing was playing");
      return;
    }
    case HostLink::Event::Restart:
      dance_.hostForget();
      Serial.printf("[viz] the computer started over (session %s)\n", link_.session());
      break;
    case HostLink::Event::Exit: {
      dance_.setHost(false);
      if (o.reply[0]) Serial.printf("%s\n", o.reply);
      const HostStats& s = link_.stats();
      const uint32_t ms = millis() - link_.enteredMs();
      Serial.printf("[viz] off (%s): %.1f s, %lu epochs, %lu hops, %lu gaps, %lu bad lines; the player stays "
                    "paused%s\n",
                    HostLink::whyName(o.why), ms / 1000.0f, static_cast<unsigned long>(s.epochs),
                    static_cast<unsigned long>(s.hops), static_cast<unsigned long>(s.gaps),
                    static_cast<unsigned long>(s.bad),
                    link_.declined() ? "; the computer is declined until it stops for 3 s" : "");
      return;
    }
    case HostLink::Event::Epoch:
      dance_.hostEpoch(o.epoch, o.rate, o.prior);
      break;
    case HostLink::Event::Prior:
      dance_.hostPrior(o.prior);
      break;
    case HostLink::Event::Hop:
      dance_.hostHop(o.hop, o.low, o.mid, o.restart, o.gap);
      break;
    case HostLink::Event::Clock:
      dance_.hostClock(nowUs, o.heard, o.playing);
      break;
    case HostLink::Event::Log:
      dance_.setHostLog(o.level);
      break;
    case HostLink::Event::None:
      break;
  }
  if (o.reply[0]) Serial.printf("%s\n", o.reply);
}
