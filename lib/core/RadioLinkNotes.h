// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The remembered headphones' radio link (their ACL, under A2DP) as BtSink's
// PlayerA2dp notes it from HCI's reports, and the link's set-up over it: the
// radio link up and A2DP not up over it yet (settingUp()), whoever opened it
// (they, or a page of ours they answered). The card's background work waits
// through it (ScanScheduler's In::btLinkSetup and btLinkSetupSeq, through
// BtSink::linkSetups()): on the device (2026-11-19, rd-g5-fix1) the walk's
// worker and an incoming set-up together took internal RAM under G5's
// floor (docs/METADATA.md 3.3.9, "The radio link's set-up").
//
// count() counts the set-up's rises and ends (free-running, odd while one
// is set up) in the call that changes it, so the loop's one read gives the
// set-up and its events together. A new set-up while one is counted counts
// two: a radio link opened over one whose close never came (its notes
// replaced), and a page of ours over a radio link up with no A2DP over it
// (A2DP opened over it, its SDP and A2DP's set-up again: a back-off's page
// over a radio link that lingers past the scheduler's cap, which then holds
// the work again from the page).
//
// The notes are about one address, the one opened() or linkUp() noted: its
// close clears them whatever the remembered addresses became since
// (closes()), and a change of the remembered address (forget(): Forget, a
// pairing's start or its failure, the console's Bn) clears them unless
// A2DP is up. A failed Connection Complete for that address while its radio
// link is noted up keeps them: a device has one radio link, so the failure
// is a page of ours that met theirs (HCI 0x0B, Connection Already Exists, or
// another code), not that link's end. A close that never reaches BtAppT (its
// queue full) leaves the set-up counted until the next radio link, the link,
// a forget(), or the scheduler's cap.
//
// Portable and single-threaded: on the Core2 every call runs on ESP32-A2DP's
// app task (BtAppT); PlayerA2dp logs what they came to and publishes
// count(). Addresses are packed (BtSink's packBda()), 0 for none.
class RadioLinkNotes {
public:
  // What a Connection Complete for the remembered headphones came to.
  enum class Opened : uint8_t {
    Answered,  // up, a page of ours open: they answered it
    Theirs,    // up, no page of ours open: they opened it themselves
    Kept,      // failed while their radio link is noted up: a page that met it, the notes kept
    Failed,    // failed, no radio link to that address noted up
  };

  // HCI's Connection Complete for a remembered address: `ok` up; `paged`,
  // a page of ours was open (connect_to(), not yet reported).
  Opened opened(uint64_t bda, bool ok, bool paged, uint32_t nowMs);
  // HCI's Disconnection Complete for `bda`: true if it was the noted radio
  // link (closes()), whose notes it clears; else nothing changes.
  bool closed(uint64_t bda);
  // A2DP up over the radio link to `bda` (noted up with it, whether its
  // open was seen or not), and A2DP down (the radio link may stay up: a
  // relink over it is no set-up).
  void linkUp(uint64_t bda);
  void linkDown();
  // A page of ours (connect_to()) to `bda`: over its radio link noted up
  // with no A2DP, a set-up again (counts two).
  void paged(uint64_t bda);
  // The remembered address changed: the notes start afresh, unless A2DP is
  // up (they are then about the linked headphones).
  void forget();
  // planStep()'s: their radio link handed to a burst of ours (once a radio
  // link: theirsWaiting() is false from then on).
  void theirsHandled() { theirs_ = false; }

  bool closes(uint64_t bda) const { return up_ && bda != 0 && bda == bda_; }
  bool up() const { return up_; }
  bool linked() const { return linked_; }  // A2DP came up over it
  // Their own radio link (no page of ours was open), A2DP not over it, not
  // yet handed to a burst (planStep() leaves A2DP to them a while).
  bool theirsWaiting() const { return up_ && theirs_ && !linked_; }
  uint32_t upAtMs() const { return upAtMs_; }
  // The link's set-up: the radio link up, A2DP not up over it (nor up at
  // all: a radio link noted while a link not yet reported down is still up
  // starts its set-up when the link goes).
  bool settingUp() const { return up_ && !linked_ && !link_; }
  uint32_t count() const { return count_; }

private:
  // count_ from settingUp() now: one when it changed; two when `again` (a
  // new set-up) finds one already counted.
  void recount(bool again);

  uint64_t bda_ = 0;
  uint32_t upAtMs_ = 0;
  uint32_t count_ = 0;
  bool up_ = false;
  bool theirs_ = false;
  bool linked_ = false;
  bool link_ = false;  // A2DP up: linkUp() to linkDown()
};
