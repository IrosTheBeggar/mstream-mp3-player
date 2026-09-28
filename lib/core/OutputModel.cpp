#include "OutputModel.h"

#include <cstdio>
#include <cstring>

const char* btPhaseName(BtLink::Phase p) {
  switch (p) {
    case BtLink::Phase::Off: return "off";
    case BtLink::Phase::Paging: return "paging";
    case BtLink::Phase::Scanning: return "scanning";
    case BtLink::Phase::Linked: return "linked";
    case BtLink::Phase::PairScan: return "pair scan";
    case BtLink::Phase::Pairing: return "pairing";
    case BtLink::Phase::Backoff: return "backoff";
    case BtLink::Phase::Resting: return "resting";
  }
  return "?";
}

const char* btCardName(BtCard c) {
  switch (c) {
    case BtCard::NotPaired: return "not paired";
    case BtCard::Off: return "off";
    case BtCard::Connecting: return "connecting";
    case BtCard::Searching: return "searching";
    case BtCard::Pairing: return "pairing";
    case BtCard::Connected: return "connected";
    case BtCard::Failed: return "failed";
    case BtCard::Lost: return "lost";
    case BtCard::Resting: return "resting";
  }
  return "?";
}

const char* btButtonLabel(BtButton b, bool narrow) {
  switch (b) {
    case BtButton::Pair: return narrow ? "Pair new" : "Pair new headphones";
    case BtButton::Connect: return "Connect";
    case BtButton::Forget: return "Forget";
    case BtButton::Cancel: return "Cancel";
    case BtButton::Disconnect: return "Disconnect";
    case BtButton::TryAgain: return "Try again";
    case BtButton::Volume: return "Volume";
    case BtButton::More: return "More";
    case BtButton::None: break;
  }
  return "";
}

// ---- BtSession ----

void BtSession::connect(uint32_t nowMs) {
  wanted_ = true;
  failed_ = false;
  pairing_ = false;
  sawTrying_ = false;
  awaitNewLink_ = false;  // linked already: that is the answer
  askedMs_ = nowMs;
}

void BtSession::cancel() {
  wanted_ = false;
  failed_ = false;
  pairing_ = false;
  sawTrying_ = false;
  awaitNewLink_ = false;
  answered_ = answeredPairing_ = false;
}

void BtSession::withdraw(bool failed) {
  if (wanted_ && pairing_) return;
  wanted_ = false;
  sawTrying_ = false;
  awaitNewLink_ = false;
  answered_ = answeredPairing_ = false;
  if (failed) failed_ = true;  // (pairing_ stays: a failed pairing's card says so)
}

void BtSession::pairStarted(uint32_t nowMs) {
  wanted_ = true;
  failed_ = false;
  pairing_ = true;
  sawTrying_ = false;
  // The headphones linked now are let go first (BtSink): until that link
  // has gone, Linked is theirs, not the new ones'.
  awaitNewLink_ = last_ == BtLink::Phase::Linked;
  answered_ = answeredPairing_ = false;
  askedMs_ = nowMs;
  pairSinceMs_ = nowMs;
}

void BtSession::update(const BtLink& link, uint32_t nowMs) {
  using P = BtLink::Phase;
  const bool linked = link.phase == P::Linked;
  if (!linked) awaitNewLink_ = false;  // the old link is gone: the next one is the answer
  if (linked && !awaitNewLink_) {
    // Satisfied (the Connected event moves the audio); a new link ends
    // whatever drop was expected of the old one.
    if (last_ != P::Linked) dropExpected_ = false;
    // A link that came up since the ask answers it (for onConnected());
    // asked while linked already, no Connected event is coming.
    if (wanted_ && last_ != P::Linked) {
      answered_ = true;
      answeredPairing_ = pairing_;
    }
    wanted_ = false;
    failed_ = false;
    pairing_ = false;
    sawTrying_ = false;
  } else if (wanted_) {
    if (link.phase == P::Paging || link.phase == P::Pairing) sawTrying_ = true;
    // The tries ran out: BtSink went on to the back-off (or rests, or
    // scans by name) after a connect's burst, or stopped (a pairing).
    const bool gaveUp = sawTrying_ && (link.phase == P::Scanning || link.phase == P::Off ||
                                       link.phase == P::Backoff || link.phase == P::Resting);
    const bool tooLong = pairing_ && static_cast<int32_t>(nowMs - pairSinceMs_) >= static_cast<int32_t>(kPairTimeoutMs);
    // None remembered: a scan by name, which has no tries to run out.
    const bool notFound = !pairing_ && !link.remembered && !sawTrying_ &&
                          static_cast<int32_t>(nowMs - askedMs_) >= static_cast<int32_t>(kFindTimeoutMs);
    if (gaveUp || tooLong || notFound) {
      wanted_ = false;
      failed_ = true;
      // pairing_ stays: the Failed card says what failed
    }
  }
  if (dropExpected_ && linked && static_cast<int32_t>(nowMs - dropSinceMs_) >= static_cast<int32_t>(kDropWaitMs)) {
    dropExpected_ = false;  // the link wasn't let go after all
  }
  last_ = link.phase;
}

BtSession::Answer BtSession::onConnected() {
  Answer a;
  a.asked = wanted_ || answered_;
  a.paired = (wanted_ && pairing_) || (answered_ && answeredPairing_);
  if (wanted_) {
    wanted_ = false;
    failed_ = false;
    pairing_ = false;
    sawTrying_ = false;
  }
  awaitNewLink_ = false;
  answered_ = answeredPairing_ = false;
  return a;
}

// ---- the card ----

BtCardView btCardView(const BtLink& link, const BtSession& session, bool lost) {
  using P = BtLink::Phase;
  BtCardView v;
  auto buttons = [&](BtButton a, BtButton b = BtButton::None, BtButton c = BtButton::None) {
    v.buttons[0] = a;
    v.buttons[1] = b;
    v.buttons[2] = c;
    v.buttonCount = (a != BtButton::None) + (b != BtButton::None) + (c != BtButton::None);
  };
  if (link.phase == P::Linked) {
    v.card = BtCard::Connected;
    v.tone = BtTone::Cyan;
    // Forget in the More sheet: not beside Disconnect and the volume.
    buttons(BtButton::Disconnect, BtButton::More, BtButton::Volume);
  } else if (lost && link.phase != P::Off && link.phase != P::Resting) {
    v.card = BtCard::Lost;
    v.tone = BtTone::Red;
    v.spinner = true;
    buttons(BtButton::Cancel);
  } else if (link.phase == P::Pairing) {
    v.card = BtCard::Pairing;
    v.tone = BtTone::Amber;
    v.spinner = true;
    buttons(BtButton::Cancel);
  } else if (session.failed()) {
    v.card = BtCard::Failed;
    v.tone = BtTone::Red;
    v.pairFailed = session.pairing();
    // A pairing that failed: pick them again, or connect to the ones still
    // remembered (Forget would forget those, not the new ones).
    if (v.pairFailed && link.remembered) {
      buttons(BtButton::TryAgain, BtButton::Connect);
    } else if (v.pairFailed) {
      buttons(BtButton::TryAgain);
    } else if (!link.remembered) {
      buttons(BtButton::TryAgain, BtButton::Pair);
    } else {
      buttons(BtButton::TryAgain, BtButton::Forget);
    }
  } else if (!link.remembered && session.wanted()) {
    // A connect with none remembered: a scan by name, on its way.
    v.card = BtCard::Searching;
    v.tone = BtTone::Amber;
    v.spinner = true;
    buttons(BtButton::Cancel);
  } else if (!link.remembered) {
    v.card = BtCard::NotPaired;
    buttons(BtButton::Pair);
  } else if (link.phase == P::Off || link.phase == P::PairScan) {
    v.card = BtCard::Off;
    buttons(BtButton::Connect, BtButton::Forget);
  } else if (link.phase == P::Resting) {
    // Not looking any more, but they come back by themselves once on:
    // no spinner, not amber (red if they dropped while the output).
    v.card = BtCard::Resting;
    v.tone = lost ? BtTone::Red : BtTone::Dim;
    v.hint = true;
    v.lostHint = lost;  // (after the whole back-off: maybe back in range, their own reconnect given up)
    buttons(BtButton::Connect);
  } else if (link.phase == P::Paging) {
    v.card = BtCard::Connecting;
    v.tone = BtTone::Amber;
    v.spinner = true;
    buttons(BtButton::Cancel);
  } else {
    v.card = BtCard::Searching;
    v.tone = BtTone::Amber;
    v.spinner = true;
    buttons(BtButton::Cancel);
  }
  return v;
}

char* btStatusLine(const BtCardView& v, const BtLink& link, const char* name, const char* detail, char* buf,
                   size_t size) {
  if (!buf || size == 0) return buf;
  char tries[24] = "";
  if (link.attempt > 0 && link.attempts > 0) {
    snprintf(tries, sizeof(tries), "  try %u of %u", static_cast<unsigned>(link.attempt),
             static_cast<unsigned>(link.attempts > link.attempt ? link.attempts : link.attempt));
  }
  const char* them = name && name[0] ? name : "headphones";
  switch (v.card) {
    case BtCard::NotPaired: snprintf(buf, size, "No headphones paired"); break;
    case BtCard::Off:
    case BtCard::Resting:  // (and the hint beside its button)
      snprintf(buf, size, "Not connected");
      break;
    case BtCard::Connecting: snprintf(buf, size, "Connecting...%s", tries); break;
    case BtCard::Searching: snprintf(buf, size, "Looking for %s...", them); break;
    case BtCard::Pairing: snprintf(buf, size, "Pairing...%s", tries); break;
    case BtCard::Connected:
      if (detail && detail[0]) {
        snprintf(buf, size, "Connected  %s", detail);
      } else {
        snprintf(buf, size, "Connected");
      }
      break;
    case BtCard::Failed:
      // Short enough for the card's line (222 px, DejaVu 13).
      snprintf(buf, size, "%s", v.pairFailed ? "Couldn't pair. In pairing mode?" : "Couldn't connect. On and nearby?");
      break;
    case BtCard::Lost:
      if (link.phase == BtLink::Phase::Paging) {
        snprintf(buf, size, "Lost: reconnecting...%s", tries);
      } else {
        snprintf(buf, size, "Lost: looking for %s...", them);
      }
      break;
  }
  return buf;
}

// ---- ConfirmTap ----

bool ConfirmTap::tap(uint32_t nowMs) {
  if (armed(nowMs)) {
    armed_ = false;
    return true;
  }
  armed_ = true;
  untilMs_ = nowMs + kWindowMs;
  return false;
}

// ---- BtScanList ----

namespace {
// A bounded copy with no formatting: note() runs on the Bluetooth task
// inside BtSink's spinlock.
void copyName(char* dst, size_t size, const char* src) {
  size_t i = 0;
  if (src) {
    for (; i + 1 < size && src[i]; ++i) dst[i] = src[i];
  }
  dst[i] = 0;
}
}  // namespace

void BtScanList::note(const uint8_t addr[6], const char* name, int rssi, uint32_t cod) {
  if (!addr) return;
  if (rssi < -127) rssi = -127;
  if (rssi > 20) rssi = 20;
  const BtDevice::Kind kind = kindOf(cod);
  for (int i = 0; i < n_; ++i) {
    BtDevice& d = d_[i];
    if (std::memcmp(d.addr, addr, 6) != 0) continue;
    d.rssi = static_cast<int8_t>(rssi);
    if (name && name[0]) copyName(d.name, sizeof(d.name), name);
    if (cod) d.kind = kind;
    return;
  }
  int at = n_;
  if (n_ == kMax) {
    // Full: the weakest makes room for a stronger one.
    at = 0;
    for (int i = 1; i < n_; ++i) {
      if (d_[i].rssi < d_[at].rssi) at = i;
    }
    if (d_[at].rssi >= rssi) return;
  } else {
    ++n_;
  }
  BtDevice& d = d_[at];
  std::memcpy(d.addr, addr, 6);
  copyName(d.name, sizeof(d.name), name);
  d.rssi = static_cast<int8_t>(rssi);
  d.kind = kind;
}

int BtScanList::bars(int rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -78) return 2;
  if (rssi >= -90) return 1;
  return 0;
}

BtDevice::Kind BtScanList::kindOf(uint32_t cod) {
  const uint32_t major = (cod >> 8) & 0x1F;
  const uint32_t minor = (cod >> 2) & 0x3F;
  if (major != 4) return BtDevice::Kind::Other;  // not Audio/Video
  switch (minor) {
    case 1:   // wearable headset
    case 2:   // hands-free
    case 6:   // headphones
      return BtDevice::Kind::Headphones;
    case 5:   // loudspeaker
    case 7:   // portable audio
    case 10:  // HiFi audio
      return BtDevice::Kind::Speaker;
    case 8:   // car audio
      return BtDevice::Kind::Car;
    default: return BtDevice::Kind::Other;
  }
}

const char* BtScanList::kindName(BtDevice::Kind k) {
  switch (k) {
    case BtDevice::Kind::Headphones: return "headphones";
    case BtDevice::Kind::Speaker: return "speaker";
    case BtDevice::Kind::Car: return "car audio";
    case BtDevice::Kind::Other: break;
  }
  return "audio device";
}
