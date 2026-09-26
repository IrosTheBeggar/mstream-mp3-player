#pragma once
#include <cstdint>

#include "VolumeMath.h"

// Decides who applies the Bluetooth volume, and what our own gain stage does.
//
// AVRCP 1.4 absolute volume, as a phone does it: the headphones render the
// volume. We (the controller) set it with SET_ABSOLUTE_VOLUME (0-127) and hear
// about their own buttons through VOLUME_CHANGED notifications. While that
// works, our gain stays at unity minus the fixed headroom (full-resolution PCM
// into SBC) and the volume the UI shows is the headphones'. When it doesn't, we
// scale the PCM ourselves (vol::softwareVolumeQ15()).
//
// What the listener hears is the headphones' own level times our gain. The
// headphones' level is unknown until they tell us (ESP-IDF drops the INTERIM
// answer to the VOLUME_CHANGED registration), and SET_ABSOLUTE_VOLUME changes
// it at once, so the rules, hearing safety first, are:
//  - Every new link starts in Software mode with the gain *snapped* to the
//    software level for the current volume, and the volume no higher than
//    kMaxLinkUpPercent (these may be other headphones).
//  - Probing: once the headphones say they report volume changes (AVRCP
//    GetCapabilities lists VOLUME_CHANGED), we send the current volume as
//    absolute volume, once per link and AVRCP connection, and audioReady()
//    holds new streams back until they answer or the timeout passes. How
//    depends on whether audio has flowed on this link (heard()) yet:
//  - Before (the pre-audio handover): the command goes out at once and our
//    gain keeps attenuating. They answer (an ACCEPT, which ESP-IDF only
//    reports for accepted ones, or a VOLUME_CHANGED at or below what we
//    sent): Absolute, and our gain is *lifted* to the headroom
//    (Actions::lift), so the first stream fades in from silence straight to
//    the right level instead of swelling up from the software level (ramped
//    up instead if a stream the headphones started has flowed meanwhile). No
//    answer within kProbeTimeoutMs: Software, and a new stream waits
//    kProbeGraceMs more.
//  - After (a late probe: the headphones' AVRCP came up after playback
//    started): the volume is capped at kMaxLinkUpPercent as on a new link and
//    our gain *dips* to silence first (ramped down while media may flow,
//    snapped while none does). The command goes out once that silence has
//    reached the headphones' output: kDuckSettleMs after the dip started, or
//    once no media has flowed for kDuckSettleMs (at once after a longer
//    pause); the silence is snapped as it goes, so the gain stage can't miss
//    it. kLateProbeTimeoutMs runs from then. The gain stays at 0 all along,
//    also for a stream that starts meanwhile. They answer: Absolute, our gain
//    ramps up from silence to the headroom (the gain stage's slow rate, not a
//    lift). No answer: Software, the gain ramps up from silence to the
//    software level.
//  - During either: the user's own volume is sent as it changes (a late
//    probe sends it with its command if that hasn't gone out yet). What
//    isn't an answer is answered by sending ours again (the cap holds): a
//    notification above what we sent, an ACCEPT above what the command it
//    answers asked for (+kEchoTolerance: a stale answer, or a level of their
//    own), or an echo above what we ask for now (+kEchoTolerance: one of an
//    older, louder command of ours, or a key of theirs that lands near one,
//    is no proof they are at ours). Only to bring them down, though (not if
//    what we ask for now is no lower than what they reported), and not in
//    reply to the answer to that very repeat unless it is a new level of
//    theirs: headphones that can't set our level (they round up by more
//    than kEchoTolerance, or have a floor) would trade commands and ACCEPTs
//    with us for ever; the probe runs out instead.
//    Each command restarts the timeout, but the probe ends at the latest
//    kProbeDeadlineFactor timeouts after its first one. An ACCEPT with no
//    command of ours waiting (more ACCEPTs than commands) is ignored.
//    A notification at or below ours that isn't an echo hands over at
//    *their* level: the UI follows it, and it is sent back once, so that a
//    command of ours still on its way can't land after it and step them up
//    (its echo is still recognised as ours). A new stream waits until that
//    is answered (its ACCEPT, or its echo), at most kProbeTimeoutMs: what
//    was on its way lands before anything is heard, not into the first
//    stream (the Powerbeats Pro answer a command ~1 s after it). During a
//    late probe's dip the silence waits for it too, and our gain only then
//    ramps up. A notification names no command, and only an ACCEPT shows
//    that one of ours has landed: while it waits, a notification near it
//    counts as its echo only once every older command of ours has an
//    ACCEPT (paired by order) and none left our history without one
//    (pushed out by later ones, or used up by a notification before the
//    wait); one near an older command with no ACCEPT answers nothing
//    either. Before that, a key of theirs near either (headphones with fine
//    steps) looks the same: it answers nothing and uses nothing up, so
//    their ACCEPTs still pair in order, and the UI doesn't follow it either:
//    echo or key, the command that waits lands after it, and they end at
//    that, the level the UI shows. Headphones that only notify (no ACCEPTs)
//    wait it out.
//  - No answer to a probe: Software, and the user's volume is no longer sent
//    at all: our gain alone follows it. Their level is unknown (they may
//    have kept their own, rejected ours, or apply absolute volume only while
//    streaming), and any command could be the first they apply, at once,
//    from their own level: even a step down on the Core2 could step them
//    up. If they confirm one of ours later (an ACCEPT for no more than it
//    asked, or an echo no louder than what we now ask for), they apply our
//    volume: Absolute. Nothing heard yet: lifted, and ours sent if it isn't
//    what they confirmed (a rise, maybe: a new stream waits for its answer,
//    as above). Otherwise they end at the last value we sent (the volume
//    shown comes down to it if the user rose since), and ours is only sent
//    if it is below what they confirmed (a step down on the Core2 since);
//    our gain ramps up at the slow rate from the software level, but only
//    once that is answered: until it lands they are at what they confirmed,
//    above the user's volume. These headphones are slow, so either wait
//    lasts as long as they took to confirm (kProbeTimeoutMs to kHoldMaxMs).
//    What isn't a confirmation (an ACCEPT above what it answers; before
//    anything was heard, also a louder notification) gets ours again as
//    above: at most the last value sent, and below what they just reported,
//    so it can't step them up; they are applying our commands then. If a
//    notification of their own comes first after audio flowed (also during
//    a probe, once a stream they started themselves has flowed), the user
//    sets their level there: if a louder command of ours may still be on
//    its way, their level is sent back once (the volume shown instead if
//    that is lower; a new stream waits for its answer, as above), then
//    nothing more is sent, and the link stays in Software.
//  - Changes made on the headphones update percent() in Absolute mode (except
//    near a command of ours on its way while one waits for an answer, above)
//    and are never sent back (except once, as themselves, above). A
//    notification is an echo of ours (it doesn't move the UI) only if it
//    matches a command they haven't confirmed yet: within kEchoTolerance of
//    what we sent (some headphones keep ~16 steps), or exactly what their
//    ACCEPT said they set, within kEchoWindowMs.
//  - AVRCP gone: Software mode, nothing sent; our gain ramps *down* (or, from
//    a dip, up from silence to the software level). A new AVRCP connection
//    gets its own probe: on a link that has played, a late one (dip, command,
//    ramp), so the volume isn't left stacked on their level for the rest of
//    the link.
//
// Invariants that follow:
//  - Once audio has flowed on a link, our gain only rises at the gain stage's
//    up rate (GainMove::Ramp, ~20 dB/s, ~2.4 s from silence to the headroom).
//    The only faster rises are the fade back to the level heard before when a
//    stream restarts after silence, and the pre-audio lift. (A stream the
//    headphones start themselves can flow before BtAppT hears of it: ESP-IDF
//    runs the data callback before STARTED reaches us, so heard() is still
//    false. A lift handed over then finds its restart used up; GainRamp drops
//    it and ramps to its target instead, from the software level to the
//    handover level at the up rate, no step: this invariant holds.)
//  - A command the listener didn't ask for (a probe) only reaches the
//    headphones while what they play from us is silent, or before anything
//    has been heard on the link: their instant change is never heard as a
//    jump. Every other command is the user's own volume, or no louder than a
//    level they already have from us or reported themselves: ours again (a
//    repeat of what is out, or lower), their own level back (at a handover,
//    or when their report ends our asking), a lower one after a late
//    confirmation. (After an unanswered probe the user's own volume is not
//    sent, see above.) One that ends a wait while a command of ours may
//    still be on its way (their level back, ours after a late confirmation)
//    holds a new stream until it is answered or its wait has passed
//    (kProbeTimeoutMs; after a late confirmation as long as they took, up to
//    kHoldMaxMs), so that what was on its way lands before the stream, not
//    into it; once audio has flowed, our gain's rise at that handover waits
//    for it the same way (at silence after a dip, else where it was).
//    None of them repeats without news: ours goes again once per
//    non-answer, not in reply to the answer to a repeat, so the commands on
//    a link are bounded by the user's and the headphones' own changes.
//  - A handover never lands above kMaxLinkUpPercent unless the user chose more
//    (with the Core2's volume, which is then sent).
//  - The headroom (setHeadroom(), a diagnostic, not saved) is applied in both
//    modes and moves by the ramp too: quickly down, slowly up.
//
// Residual risks (latency): commands reach the headphones in order but maybe
// late, and ESP-IDF reports only ACCEPTs, naming no command.
//  - A probe applied after its timeout (and kProbeGraceMs), once audio plays
//    in Software mode, steps their level from their own up to what we sent
//    for p, the volume then: heard as their level for p times our gain at
//    that moment (the software level for the volume shown, which the user
//    may have raised since; still rising from silence after a late probe),
//    never above what Absolute gives for p. Its ACCEPT or echo then hands
//    over by the ramp. The random-events test's headset lands commands
//    seconds late and checks this bound. The same holds for one still on its
//    way when a stream held for the answer to a later one (sendHeld())
//    starts after its wait without it, and for one still on its way when a
//    rise held for such an answer (a late handover, their report ending a
//    dip) goes ahead after that wait: our gain then rises by the ramp while
//    their level may still be the one they confirmed, at most what absolute
//    volume gives for the volume sent then (headphones slower than they
//    were, or than kHoldMaxMs).
//  - The answer that releases a held stream names no command either. A
//    notification within kEchoTolerance of the held command (or of a later
//    one) counts once every older command of ours has an ACCEPT, and none
//    left the history without one: then the echo of an older one near it
//    (the held one, landing in the stream, is within that tolerance of it),
//    or a key of theirs near it (the held one may still land in the
//    stream, within kEchoTolerance of that key). An ACCEPT paired with it
//    by order that isn't near it (a command they dropped, a stale answer)
//    leaves it to the wait. Commands that left the history with no ACCEPT
//    make the pairing late: the ACCEPT paired with the held one is then
//    that of the command just before it (one left: only the held one may
//    still land, as above), or of an older one (more than one: one after
//    that may still land too, bounded as the first risk above).
//  - Their level sent back at a handover may land after a further key press
//    of theirs, taking them back to the level they had just reported.
//  - During a probe, an ACCEPT of an older command of ours hands over while
//    a lower one is still on its way: until it lands they stay at the older
//    level (the user's own a moment ago), and our gain meanwhile only rises
//    by the ramp. (After a probe, see the late confirmation above: the rise
//    waits for the lower one.)
//  - An ACCEPT for a command from before the probe that asked no more than
//    it can't be told from the probe's own: it hands over, and the probe,
//    landing after it, sets our level.
//  - In Absolute mode the UI follows their reports. One that overtakes a
//    command of ours still on its way (a key of theirs, or after it the
//    echo of an older command, no longer recognised as ours: also an echo
//    more than kEchoWindowMs after its command, as from headphones slow
//    enough to confirm late) leaves the UI above their level once ours
//    lands, until they report again; a step down from it then sends more
//    than they have: the user's own volume, as shown. The random-events
//    test counts these steps instead of checking them. While a command
//    waits for an answer (sendHeld()), a notification near one of ours on
//    its way doesn't move the UI; if that one then isn't the last to land
//    (the one that waits was dropped, or a key of theirs came after it
//    landed, reported before an older one's ACCEPT), the UI stays at the
//    level that waits instead of theirs, until they report again.
//
// Portable and single-threaded: the caller serialises calls (on the Core2 all
// of them run on ESP32-A2DP's app task, BtAppT, through BtControl) and passes a
// millisecond clock (wraparound is fine). Each call returns what the caller
// has to do.
class AbsVolumePolicy {
public:
  enum class Mode : uint8_t {
    Software,  // we scale the PCM
    Probing,   // asking the headphones to take over; still scaling the PCM (or silent, late)
    Absolute,  // the headphones apply the volume; our gain is the fixed headroom
  };

  struct Actions {
    bool sendAbsolute = false;  // send SET_ABSOLUTE_VOLUME(absolute)
    uint8_t absolute = 0;
    bool gainChanged = false;   // request gainQ15() from the gain stage
    bool snap = false;          //   ...at once, only ever downwards
    bool lift = false;          //   ...at the next stream start, upwards (nothing heard on this link)
    bool volumeChanged = false; // percent() changed without userSet(): refresh the UI
    bool modeChanged = false;   // mode() changed (worth a log line)
    bool probeUnanswered = false;  // the probe timed out (worth a log line)
  };

  // How long a probe's command may take to be answered; also how long a new
  // stream waits for the answer to a command that ended a wait (sendHeld()).
  static constexpr uint32_t kProbeTimeoutMs = 2000;
  // After a late confirmation (the probe went unanswered in time), that wait,
  // and our gain's rise with it, lasts as long as they took to confirm: at
  // most this.
  static constexpr uint32_t kHoldMaxMs = 6000;
  // A probe ends at the latest this many timeouts after its first command,
  // however often a new command of it (the user's volume, ours again) has
  // restarted the timeout since: a new stream isn't held back for long.
  static constexpr uint32_t kProbeDeadlineFactor = 2;
  // After a probe that went unanswered before anything was heard, a new
  // stream waits this much longer: headphones that apply it late then do so
  // before the first stream rather than into it.
  static constexpr uint32_t kProbeGraceMs = 1000;
  // A late probe: from the start of the dip, or from when media stopped
  // flowing, until the command may go out (the ~23 ms fade down, ESP-IDF's
  // queue of encoded frames, which grows on a congested link, the
  // headphones' own ~150 ms buffer, and a wide margin: a longer wait only
  // costs silence), and how long its answer may take (the listener hears
  // silence meanwhile).
  static constexpr uint32_t kDuckSettleMs = 800;
  static constexpr uint32_t kLateProbeTimeoutMs = 1000;
  static constexpr uint32_t kEchoWindowMs = 1500;
  static constexpr uint8_t kEchoTolerance = 4;  // half a step of 16-step headphones
  static constexpr uint8_t kMaxLinkUpPercent = 60;
  // How long a new link waits for the AVRCP capabilities before audio may
  // start; longer once AVRCP is connected and they are on their way.
  static constexpr uint32_t kCapsWaitMs = 1500;
  static constexpr uint32_t kCapsReplyWaitMs = 3000;

  explicit AbsVolumePolicy(uint8_t percent = 30);

  // A2DP link up / down. linkUp() on a link that is up changes nothing.
  Actions linkUp(uint32_t nowMs);
  Actions linkDown();
  // AVRCP connected (its capabilities follow).
  void avrcpUp(uint32_t nowMs);
  // AVRCP: the headphones' GetCapabilities answer (does it list VOLUME_CHANGED?).
  // May arrive before or after linkUp(); kept while AVRCP stays connected,
  // also across an A2DP drop.
  Actions capabilities(bool volumeChange, uint32_t nowMs);
  // AVRCP gone (the A2DP link may stay): back to software volume, ramped down.
  Actions avrcpDown();
  // Media may flow (a START is out, the stream runs or is being suspended).
  // Once it has on a link, a handover on it is a late one (a dip first).
  Actions streamActive(bool active, uint32_t nowMs);
  // SET_ABSOLUTE_VOLUME accepted; `absolute` is what they actually set.
  Actions accepted(uint8_t absolute, uint32_t nowMs);
  // VOLUME_CHANGED notification: the headphones' volume is now `absolute`.
  Actions headsetChanged(uint8_t absolute, uint32_t nowMs);
  // Volume set on the Core2 (buttons, console, headphone VOL keys).
  Actions userSet(uint8_t percent, uint32_t nowMs);
  // Timers; call a few times a second (only matters while Probing).
  Actions tick(uint32_t nowMs);
  // Our fixed attenuation (Q15, at most unity), vol::kHeadroomQ15 until set.
  Actions setHeadroom(uint16_t q15);

  uint8_t percent() const { return percent_; }
  Mode mode() const { return mode_; }
  uint16_t gainQ15() const { return gain_; }        // what the gain stage should be at
  int headsetAbsolute() const { return headsetAbs_; }  // last reported by the headphones, -1 unknown
  bool linked() const { return linked_; }
  bool heard() const { return heard_; }  // media has flowed on this link
  bool ducked() const { return ducked_; }  // a late probe (or its handover, see sendHeld()): our gain is at silence
  uint16_t headroomQ15() const { return headroom_; }
  // A new media stream may start: not while a probe is out, not while a
  // command that ended a wait is unanswered (at most kProbeTimeoutMs, or
  // kHoldMaxMs after a late confirmation), and
  // not while the capabilities of a new link are still on their way (see
  // kCapsWaitMs).
  bool audioReady(uint32_t nowMs) const;

private:
  static constexpr int kSentHistory = 4;
  struct Sent {
    uint8_t absolute = 0;
    int16_t accepted = -1;  // what their ACCEPT said they set, -1 not yet
    uint32_t ms = 0;
    bool valid = false;
    bool holds = false;  // a new stream waits for its answer (see sendHeld())
  };

  uint16_t targetGain() const;
  void retarget(Actions& a, bool snap);
  void setMode(Actions& a, Mode m);
  void setPercent(Actions& a, uint8_t percent);
  void send(Actions& a, uint32_t nowMs);
  void sendValue(Actions& a, uint8_t absolute, uint32_t nowMs, bool again = false);
  void sendHeld(Actions& a, uint8_t absolute, uint32_t nowMs, uint32_t waitMs = kProbeTimeoutMs);
  uint32_t lateWaitMs(uint32_t nowMs) const;
  void releaseHold(Actions& a, uint32_t nowMs);
  bool askAgain(Actions& a, uint32_t nowMs, bool newLevel);
  void maybeProbe(Actions& a, uint32_t nowMs);
  bool silentAtHeadphones(uint32_t nowMs) const;
  void sendIfSilent(Actions& a, uint32_t nowMs);
  void sendProbe(Actions& a, uint32_t nowMs);
  void handOver(Actions& a);
  void handOverPlaying(Actions& a);
  void confirmLate(Actions& a, uint8_t absolute, uint32_t nowMs);
  void endDip();
  bool canHandOver() const { return !heard_ && !streaming_; }
  // What a notification is to takeEcho(): a level of theirs, the echo of a
  // command of ours, or, while a command is held, one near a command of ours
  // that may still be on its way (see takeEcho()).
  enum class Heard : uint8_t { Theirs, Echo, Held };
  Heard takeEcho(uint8_t absolute, uint32_t nowMs, bool keepOnMiss);
  bool unansweredBeforeHeld(int i) const;
  int noteAccepted(uint8_t absolute);
  uint8_t askedLevel() const;
  void dropSent(int i);
  void clearSent();

  uint8_t percent_;
  uint16_t headroom_ = vol::kHeadroomQ15;
  Mode mode_ = Mode::Software;
  uint16_t gain_;
  bool linked_ = false;
  bool avrcUp_ = false;
  bool capsKnown_ = false;  // the capabilities arrived on this AVRCP connection
  bool capable_ = false;    // they list VOLUME_CHANGED
  bool probed_ = false;     // probe already tried on this link / AVRCP connection
  bool awaitingLate_ = false;  // Software after an unanswered probe: a late answer still hands over
  bool streaming_ = false;
  bool heard_ = false;      // media has flowed on this link
  bool ducked_ = false;     // a late probe: our gain held at silence
  bool sendPending_ = false;  // ...whose command waits for the silence to reach the headphones
  uint32_t linkUpMs_ = 0;
  uint32_t avrcUpMs_ = 0;
  uint32_t probeStartMs_ = 0;  // when the last command of the probe went out
  uint32_t probeFirstMs_ = 0;  // ...and its first (the deadline runs from there)
  uint32_t unansweredMs_ = 0;  // when the probe timed out
  uint32_t duckMs_ = 0;        // when the dip started
  uint32_t stopMs_ = 0;        // when media last stopped flowing
  int headsetAbs_ = -1;
  int lastSent_ = -1;  // the last absolute volume sent on this link / AVRCP connection
  bool askedAgain_ = false;  // ...was ours again, in reply to something that wasn't an answer
  bool held_ = false;        // a new stream waits for the answer to a command (see sendHeld())
  uint32_t heldMs_ = 0;      // ...sent then
  uint32_t heldForMs_ = kProbeTimeoutMs;  // ...for at most this long
  bool riseHeld_ = false;    // ...and so does our gain's rise after a late handover
  uint16_t heldGain_ = 0;    // ...kept meanwhile at this (or at silence, ducked_)
  uint32_t answeredMs_ = 0;  // when the command the last ACCEPT or echo answered went out
  Sent sent_[kSentHistory];  // oldest first
  // A command of ours left sent_ with no ACCEPT (shifted out, or used up by
  // a notification) on this link / AVRCP connection: it may still be on its
  // way, and a notification can no longer release a hold (see sendHeld()).
  bool lostUnanswered_ = false;
};
