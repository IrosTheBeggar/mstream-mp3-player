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
// answer to the VOLUME_CHANGED registration), so the rules, hearing safety
// first, are:
//  - Every new link starts in Software mode with the gain *snapped* to the
//    software level for the current volume, and the volume no higher than
//    kMaxLinkUpPercent (these may be other headphones).
//  - The handover to the headphones (Absolute mode, our gain up to the
//    headroom) only happens while nothing has been heard on this link yet:
//    the level it lands on is then the first level heard, not a rise. Once
//    audio has flowed on a link (heard()), our gain never rises and nothing
//    is sent to the headphones except through userSet(). Headphones found
//    capable too late (capabilities, a notification or an ACCEPT after audio
//    flowed) stay in Software mode until the next link.
//  - Probing: once the headphones say they report volume changes (AVRCP
//    GetCapabilities lists VOLUME_CHANGED), we send the current volume as
//    absolute volume but keep attenuating, and audioReady() holds the stream
//    back until they answer or kProbeTimeoutMs passes.
//  - They answer (an ACCEPT, which ESP-IDF only reports for accepted ones, or
//    a VOLUME_CHANGED at or below what we sent): Absolute. Our gain is
//    *lifted* to the headroom (Actions::lift): no audio has flowed, so the
//    first stream fades in from silence straight to the right level instead
//    of swelling up from the software level. A notification above what we
//    sent is answered by sending ours again (the link-up cap holds).
//  - No answer within kProbeTimeoutMs: Software, but the volume is still sent
//    on each userSet() (headphones that apply it silently then stack it with
//    our gain: quieter, never louder). Until one of the headphones' own
//    changes shows the user sets their level there: then nothing more is sent.
//  - Changes made on the headphones update percent() in Absolute mode and are
//    never sent back. A notification is an echo of ours (it doesn't move the
//    UI) only if it matches a command they haven't confirmed yet: within
//    kEchoTolerance of what we sent (some headphones keep ~16 steps), or
//    exactly what their ACCEPT said they set, within kEchoWindowMs.
//  - AVRCP gone: Software mode, nothing sent; our gain ramps *down*.
//
// Portable and single-threaded: the caller serialises calls (on the Core2 all
// of them run on ESP32-A2DP's app task, BtAppT, through BtControl) and passes a
// millisecond clock (wraparound is fine). Each call returns what the caller
// has to do.
class AbsVolumePolicy {
public:
  enum class Mode : uint8_t {
    Software,  // we scale the PCM
    Probing,   // asked the headphones to take over; still scaling the PCM
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
  // Once it has on a link, that link never hands over.
  Actions streamActive(bool active, uint32_t nowMs);
  // SET_ABSOLUTE_VOLUME accepted; `absolute` is what they actually set.
  Actions accepted(uint8_t absolute, uint32_t nowMs);
  // VOLUME_CHANGED notification: the headphones' volume is now `absolute`.
  Actions headsetChanged(uint8_t absolute, uint32_t nowMs);
  // Volume set on the Core2 (buttons, console, headphone VOL keys).
  Actions userSet(uint8_t percent, uint32_t nowMs);
  // Timeouts; call now and then (only matters while Probing).
  Actions tick(uint32_t nowMs);

  uint8_t percent() const { return percent_; }
  Mode mode() const { return mode_; }
  uint16_t gainQ15() const { return gain_; }        // what the gain stage should be at
  int headsetAbsolute() const { return headsetAbs_; }  // last reported by the headphones, -1 unknown
  bool linked() const { return linked_; }
  bool heard() const { return heard_; }  // media has flowed on this link
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
  void handOver(Actions& a);
  bool canHandOver() const { return !heard_ && !streaming_; }
  bool takeEcho(uint8_t absolute, uint32_t nowMs);
  void noteAccepted(uint8_t absolute);
  void clearSent();

  uint8_t percent_;
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
  uint32_t linkUpMs_ = 0;
  uint32_t avrcUpMs_ = 0;
  uint32_t probeStartMs_ = 0;
  int headsetAbs_ = -1;
  Sent sent_[kSentHistory];  // oldest first
};
