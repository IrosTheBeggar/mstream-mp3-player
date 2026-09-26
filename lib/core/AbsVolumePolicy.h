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
//    with us for ever; the probe runs out instead, and a later step down
//    that they do set confirms.
//    Each command restarts the timeout, but the probe ends at the latest
//    kProbeDeadlineFactor timeouts after its first one. An ACCEPT with no
//    command of ours waiting (more ACCEPTs than commands) is ignored.
//    A notification at or below ours that isn't an echo hands over at
//    *their* level: the UI follows it, and it is sent back once, so that a
//    command of ours still on its way can't land after it and step them up
//    (its echo is still recognised as ours).
//  - No answer to a probe: Software, and only steps *down* are still sent on
//    userSet() (to at most the last value sent): headphones that apply them
//    silently stack them with our gain, quieter, never louder; a rise is
//    left to our gain alone. If they confirm one of ours later (an ACCEPT for
//    no more than it asked, or an echo no louder than what we now ask for),
//    they apply our volume: Absolute. Nothing heard yet: lifted, and ours
//    sent if it isn't what they confirmed. Otherwise our gain ramps up at the
//    slow rate from the software level, they end at the last value we sent
//    (the volume shown comes down to it if the user rose since), and ours is
//    only sent if it is below what they confirmed. What isn't a confirmation
//    gets ours again as above, at most the last value sent. If a
//    notification of their own comes first after audio flowed (also during
//    a probe, once a stream they started themselves has flowed), the user
//    sets their level there: their level is sent back once if a louder
//    command of ours may still be on its way, then nothing more is sent, and
//    the link stays in Software.
//  - Changes made on the headphones update percent() in Absolute mode and are
//    never sent back (except once, as themselves, above). A
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
//    confirmation. None of them repeats without news: ours goes again once
//    per non-answer, not in reply to the answer to a repeat, so the
//    commands on a link are bounded by the user's and the headphones' own
//    changes.
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
//    seconds late and checks this bound.
//  - Their level sent back at a handover may land after a further key press
//    of theirs, taking them back to the level they had just reported.
//  - An ACCEPT of an older command of ours hands over while a lower one is
//    still on its way: until it lands they stay at the older (the user's
//    own) level, and our gain meanwhile only rises by the ramp.
//  - An ACCEPT for a command from before the probe that asked no more than
//    it can't be told from the probe's own: it hands over, and the probe,
//    landing after it, sets our level.
//  - In Absolute mode the UI follows their reports. One that overtakes a
//    command of ours still on its way (a key of theirs, or after it the
//    echo of an older command, no longer recognised as ours) leaves the UI
//    above their level once ours lands, until they report again; a step
//    down from it then sends more than they have: the user's own volume, as
//    shown. The random-events test counts these steps instead of checking
//    them.
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

  static constexpr uint32_t kProbeTimeoutMs = 2000;
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
  bool ducked() const { return ducked_; }  // a late probe: our gain is at silence
  uint16_t headroomQ15() const { return headroom_; }
  // A new media stream may start: not while a probe is out, and not while
  // the capabilities of a new link are still on their way (see kCapsWaitMs).
  bool audioReady(uint32_t nowMs) const;

private:
  static constexpr int kSentHistory = 4;
  struct Sent {
    uint8_t absolute = 0;
    int16_t accepted = -1;  // what their ACCEPT said they set, -1 not yet
    uint32_t ms = 0;
    bool valid = false;
  };

  uint16_t targetGain() const;
  void retarget(Actions& a, bool snap);
  void setMode(Actions& a, Mode m);
  void setPercent(Actions& a, uint8_t percent);
  void send(Actions& a, uint32_t nowMs);
  void sendValue(Actions& a, uint8_t absolute, uint32_t nowMs, bool again = false);
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
  bool takeEcho(uint8_t absolute, uint32_t nowMs, bool keepOnMiss);
  int noteAccepted(uint8_t absolute);
  uint8_t keepCeiling() const;
  uint8_t askedLevel() const;
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
  bool keepSending_ = false;  // Software after an unanswered probe: still send the volume
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
  Sent sent_[kSentHistory];  // oldest first
};
