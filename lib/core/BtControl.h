#pragma once
#include <cstdint>

#include "AbsVolumePolicy.h"
#include "StreamControl.h"

// The Bluetooth output's decisions in one place: the media stream
// (StreamControl) and the volume (AbsVolumePolicy), and how they depend on
// each other. A probe of the headphones' volume holds a new stream back until
// it is answered (a stream already running goes on); once media may have
// flowed on a link (a START out or answered, the stream running or being
// suspended), a probe first dips our gain to silence. BtSink's PlayerA2dp
// feeds it the Bluetooth stack's events and carries out what comes back
// through Io.
//
// Portable and single-threaded: on the Core2 every call runs on ESP32-A2DP's
// app task (BtAppT). Millisecond clock, wraparound-safe.
class BtControl {
public:
  enum class GainMove : uint8_t {
    Ramp,  // the gain stage's own rates (quick down, slow up)
    Snap,  // down at once (nothing is playing)
    Lift,  // up to this at the next stream start (nothing heard on this link)
  };

  // What the control asks of the Bluetooth stack and the rest of the firmware.
  class Io {
  public:
    // Issue an A2DP media-control command. false: it could not be queued.
    virtual bool mediaCtrl(StreamControl::Cmd cmd) = 0;
    // AVRCP SET_ABSOLUTE_VOLUME (only called while AVRCP is connected).
    virtual void sendAbsoluteVolume(uint8_t absolute) = 0;
    virtual void setGain(uint16_t q15, GainMove move) = 0;
    // The volume changed by itself (the headphones, or capped on link-up).
    virtual void volumeChanged() = 0;
    // Who applies the volume changed (worth a log line).
    virtual void volumeModeChanged(AbsVolumePolicy::Mode mode, bool probeUnanswered) = 0;
    // The headphones stopped our stream themselves: pause the player.
    virtual void remoteSuspend() = 0;
    // A media command went unanswered (worth a log line).
    virtual void commandGaveUp() = 0;
    // The stream started this long after playback asked for it.
    virtual void streamStarted(uint32_t afterMs) = 0;
    // Called at the end of every entry point: publish state for other tasks.
    virtual void publish() = 0;

  protected:
    ~Io() = default;
  };

  BtControl(Io& io, uint8_t percent) : io_(io), policy_(percent) {}

  // A2DP link. linkUp() on a link that is up changes nothing.
  void linkUp(uint32_t nowMs);
  void linkDown(uint32_t nowMs);
  // AVRCP controller connection, the headphones' notification capabilities,
  // their answers and notifications.
  void avrcpUp(uint32_t nowMs);
  void avrcpDown(uint32_t nowMs);
  void capabilities(bool volumeChange, uint32_t nowMs);
  void accepted(uint8_t absolute, uint32_t nowMs);
  void headsetChanged(uint8_t absolute, uint32_t nowMs);
  // Volume from the Core2 (buttons, console, headphone VOL keys).
  void setVolume(uint8_t percent, uint32_t nowMs);
  void stepVolume(int delta, uint32_t nowMs);
  // Our fixed attenuation (Q15), a diagnostic.
  void setHeadroom(uint16_t q15, uint32_t nowMs);
  // Playback wants the stream; suspend at once once it doesn't.
  void setWanted(bool wanted, uint32_t nowMs);
  void suspendNow(uint32_t nowMs);
  // The stack's media-control answers and audio states.
  void mediaAck(StreamControl::Cmd cmd, bool ok, uint32_t nowMs);
  void audioState(bool started, uint32_t nowMs);
  // Timers. A few times a second.
  void tick(uint32_t nowMs);

  const AbsVolumePolicy& volume() const { return policy_; }
  const StreamControl& stream() const { return stream_; }
  bool avrcpConnected() const { return avrcUp_; }

private:
  void act(const StreamControl::Actions& a, uint32_t nowMs);
  void apply(const AbsVolumePolicy::Actions& a);
  void settle(uint32_t nowMs);

  Io& io_;
  AbsVolumePolicy policy_;
  StreamControl stream_;
  bool avrcUp_ = false;
  bool policyActive_ = false;  // what policy_ was last told about the stream
  bool gateOpen_ = true;       // what stream_ was last told about policy_.audioReady()
};
