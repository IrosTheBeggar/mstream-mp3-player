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
//    answer within kProbeTimeoutMs: Software.
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
//  - During either: a notification above what we sent is answered by sending
//    ours again (the cap holds); the user's own volume is sent as it changes
//    (a late probe sends it with its command if that hasn't gone out yet).
//  - No answer to a probe: Software, but the volume is still sent on each
//    userSet() (headphones that apply it silently then stack it with our
//    gain: quieter, never louder). If they confirm one of ours later (an
//    ACCEPT, or an echo), they apply our volume: Absolute (lifted if nothing
//    has been heard yet, else ramped up at the slow rate from the software
//    level). If a notification of their own comes first, the user sets their
//    level there: nothing more is sent, and the link stays in Software.
//  - Changes made on the headphones update percent() in Absolute mode and are
//    never sent back. A notification is an echo of ours (it doesn't move the
//    UI) only if it matches a command they haven't confirmed yet: within
//    kEchoTolerance of what we sent (some headphones keep ~16 steps), or
//    exactly what their ACCEPT said they set, within kEchoWindowMs.
//  - AVRCP gone: Software mode, nothing sent; our gain ramps *down* (or, from
//    a dip, up from silence to the software level). A new AVRCP connection
//    gets its own probe.
//
// Invariants that follow:
//  - Once audio has flowed on a link, our gain only rises at the gain stage's
//    up rate (GainMove::Ramp, ~20 dB/s, ~2.4 s from silence to the headroom).
//    The only faster rises are the fade back to the level heard before when a
//    stream restarts after silence, and the pre-audio lift.
//  - A command the listener didn't ask for (a probe, or ours again after a
//    louder notification) only reaches the headphones while what they play
//    from us is silent, or before anything has been heard on the link: their
//    instant change is never heard as a jump. (Otherwise only the user's own
//    volume is sent: when it changes, or again after a late confirmation.)
//  - A handover never lands above kMaxLinkUpPercent unless the user chose more
//    (with the Core2's volume, which is then sent).
//  - The headroom (setHeadroom(), a diagnostic, not saved) is applied in both
//    modes and moves by the ramp too: quickly down, slowly up.
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
  void maybeProbe(Actions& a, uint32_t nowMs);
  bool silentAtHeadphones(uint32_t nowMs) const;
  void sendIfSilent(Actions& a, uint32_t nowMs);
  void sendProbe(Actions& a, uint32_t nowMs);
  void handOver(Actions& a);
  void handOverPlaying(Actions& a);
  void endDip();
  bool canHandOver() const { return !heard_ && !streaming_; }
  bool takeEcho(uint8_t absolute, uint32_t nowMs);
  void noteAccepted(uint8_t absolute);
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
  uint32_t duckMs_ = 0;        // when the dip started
  uint32_t stopMs_ = 0;        // when media last stopped flowing
  int headsetAbs_ = -1;
  Sent sent_[kSentHistory];  // oldest first
};
