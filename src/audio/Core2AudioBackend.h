#pragma once
#include <FS.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "ClickGen.h"
#include "PcmRing.h"
#include "ToneGen.h"
#include "TransportSync.h"
#include "UiBoost.h"
#include "audio/AudioShared.h"
#include "audio/BtSink.h"
#include "audio/SpeakerSink.h"
#include "hal/IAudioBackend.h"

class AudioFileSourceFS;
class AudioFileSourceID3;
class AudioGenerator;
class AudioGeneratorFLAC;
class AudioGeneratorMP3;
class AudioOutput;
class RingOutput;

// IAudioBackend for the Core2. A decode task turns the current track into PCM
// in a PSRAM ring (PcmRing); the active output, Bluetooth headphones or the
// internal speaker, plays from the ring. play() and stop() are requests: they
// post a new generation to TransportSync and wake the decode task, and
// finished()/failed() only ever describe the latest request.
//
// Tracks are .mp3/.flac files on the library filesystem, or the built-in test
// tones "tone:440", "tone:1000" and "tone:left" (440 Hz, left channel only),
// and 60 s click tracks with a known beat, "tone:click<bpm>" and
// "tone:click<bpm>off" (first beat 0.37 of a period in; see ClickGen).
class Core2AudioBackend : public IAudioBackend {
public:
  enum class Output : uint8_t { Speaker, Bluetooth };

  struct Stats {
    uint32_t bufferedMs;       // audio waiting in the ring
    uint32_t underruns;        // gaps the active output had to fill with silence
    uint32_t btFramesPerSec;   // pulled by the Bluetooth stack, ~44100 while streaming
    float decodeLoad;          // time spent producing / audio produced, current track
    uint32_t decodeStackFree;  // bytes never used by the decode task
  };

  Core2AudioBackend();
  ~Core2AudioBackend() override;  // out of line: the decoder types are incomplete here

  // Allocates the ring, starts Bluetooth (headphones named `btSinkName`, see
  // BtSink), the speaker pump and the decode task. `fs` is the library
  // (nullptr: tones only).
  bool begin(fs::FS* fs, const char* btSinkName);

  // IAudioBackend
  bool play(const std::string& path, uint32_t durationHintMs) override;
  void pause() override;
  void resume() override;
  void stop() override;
  void loop(uint32_t nowMs) override;
  bool isPlaying() const override;
  uint32_t positionMs() const override;
  bool finished() const override;
  bool failed() const override;

  // Decodes up to 20 s of `path` as fast as possible, output discarded, and
  // prints how many times faster than realtime that was. Stops playback.
  void bench(const std::string& path);

  void setOutput(Output output);
  Output output() const { return output_; }
  // The active output's volume, 0-100 %. The speaker and Bluetooth keep their
  // own, like a phone: with absolute volume the Bluetooth one is the
  // headphones' own volume (their buttons change it), and a loud speaker
  // setting must never be sent to headphones.
  void setVolume(uint8_t percent);
  // Up or down from the active output's current volume. On Bluetooth the
  // step is applied where the volume lives (BtSink), so quick presses and
  // headphone changes in between are never lost.
  void stepVolume(int delta);
  uint8_t volume() const;
  // The speaker's volume whichever output is active: silent test mode mutes
  // it before the speaker takes the ring, so no buffer plays at the old level.
  void setSpeakerVolume(uint8_t percent);

  BtSink& bluetooth() { return bt_; }
  // What an output just played, for the beat tracker (nullptr: no PSRAM).
  const AudioTap* tap(Output output) const { return output == Output::Bluetooth ? bt_.tap() : speaker_.tap(); }
  // How long after an output's tap write its audio is heard, and how that
  // was found (for the logs): Bluetooth, the headphones' delay report plus
  // ~25 ms for ESP-IDF's queue and the radio; the speaker, its measured queue
  // plus the I2S DMA. Loop task.
  uint32_t outputLatencyUs(Output output, char* how, size_t howLen) const;
  // Sample rate of the audio in the ring (the speaker plays at it).
  int sampleRate() const { return shared_.rate; }
  const Stats& stats() const { return stats_; }  // refreshed by loop() once a second
  // Live numbers for the UI (any task; stats() is once a second):
  // audio waiting in the ring now, in ms of the ring's sample rate. The
  // scrolling lists back off when it runs low (ScrollGovernor).
  uint32_t bufferedMsNow() const;
  // The outputs' underrun count (free-running).
  uint32_t underrunsNow() const { return shared_.underruns.load(std::memory_order_relaxed); }
  // Decode task time spent producing, since boot (never reset; per-second
  // deltas give the decoder's share of core 1, SD waits included).
  uint64_t decodeBusyUsTotal() const { return busyTotalUs_.load(std::memory_order_relaxed); }
  // Whether a low ring now means the audio is at risk: true once the
  // current track has filled the ring to kSteadyMs, false from a track's
  // start until then (the ring is filling from empty) and while it drains at
  // the end of a file (the ring empties on purpose). The scrolling lists
  // only back off while this is true (ScrollGovernor); an underrun counts
  // whatever it says.
  bool ringSteady() const { return ringSteady_.load(std::memory_order_relaxed); }
  static constexpr uint32_t kSteadyMs = 1000;

  // ---- Sharing core 1 with the UI (docs/UI-SPIKE.md, "Scroll round 2") ----
  //
  // The decode task runs at kDecodePriority, above the Arduino loop (1), so
  // the UI gets what the decoder leaves. Two opt-in changes to that, both
  // off by default (the scroll lab turns them on for its A/B runs; the real
  // UI will too):
  //
  // Interaction boost. While enabled, the UI calls noteUiActivity() every
  // pass while the user interacts (finger down, list moving). A 10 ms
  // esp_timer, on the esp_timer task (core 0, so it runs however busy core 1
  // is), applies UiBoost's rule: while the interaction lasts (and 300 ms
  // after) and the ring holds at least 900 ms, it drops the decode task to
  // kDecodePriorityYielding (0, below the loop), so the decoder only runs
  // when the loop sleeps (its delay(5) per pass, its frame cap and its rest
  // after each frame), and then shares core 1 with IDLE1 (also priority 0,
  // time-sliced), so it gets about half of that sleep; the ring pays for
  // the difference. Below 900 ms it restores kDecodePriority at once, and
  // boosts again only from 1,200 ms and at least 300 ms after the drop.
  // Nothing else changes: the outputs, the speaker pump (3) and
  // M5.Speaker's task (2) keep their priorities, all above the loop. The UI
  // must still sleep every frame (the scroll lab rests >= 20 ms after each).
  void setUiBoost(bool enabled);  // loop task
  bool uiBoostEnabled() const { return boostEnabled_.load(std::memory_order_relaxed); }
  void noteUiActivity(uint32_t nowMs);  // any task
  struct BoostStatus {
    bool on;            // the decode task yields to the UI now
    UiBoost::Why why;   // why, at the last change
    uint32_t changes;   // on/off changes since boot (free-running)
    uint32_t ringMs;    // the ring at the last change
    uint32_t atMs;      // when (millis)
  };
  BoostStatus boostStatus() const;
  static constexpr UBaseType_t kDecodePriority = 2;
  static constexpr UBaseType_t kDecodePriorityYielding = 0;

  // Gentle refill. At a track start or skip the decoder fills the whole
  // ~1.45 s ring flat out, which on core 1 starves the UI for ~0.7 s (an
  // MP3). With pacing on, during that fill (until the ring is first full)
  // and once the ring holds gentleFromMs (500 ms), the decode task sleeps
  // after each pass so that it produces at most capX10/10 times realtime
  // (RefillPacer; capX10 is at least 15, 1.5x); below 500 ms it
  // runs flat out as before, so the time to first audio doesn't change.
  // Refills later in the track (after a dip) are never paced.
  void setRefillPacing(bool enabled, uint32_t capX10);  // any task
  RefillPacer::Config refillPacing() const;

  // How the last start or skip filled the ring, ms after the request
  // (play()); -1 not yet. The decode task fills it in; loop() logs it once
  // the ring is full ("[audio] refill: ...").
  struct StartTiming {
    uint32_t seq;          // counts starts (free-running)
    int32_t firstAudioMs;  // the first frames in the ring (time to first audio, before the output's own latency)
    int32_t ring500Ms;     // 500 ms buffered
    int32_t steadyMs;      // kSteadyMs buffered (ringSteady())
    int32_t fullMs;        // the ring full (the decoder waits for room)
  };
  StartTiming startTiming() const;
  // For the UI; set by the decode task. Title/artist come from ID3 tags and are
  // empty when the file has none.
  std::string description() const;  // e.g. "MP3, 44100 Hz"
  std::string trackTitle() const;
  std::string trackArtist() const;
  std::string note() const;  // why the last track failed, or ""

private:
  enum class Work : uint8_t { Idle, Producing, Draining };
  enum class Produced : uint8_t { More, Done, Failed };
  enum class Kind : uint8_t { Play, Stop, Bench };
  struct Request {
    std::string path;
    Kind kind = Kind::Stop;
    uint32_t pauses = 0;  // pauses_ when it was made
  };

  static void taskEntry(void* self);
  static void onMetadata(void* self, const char* type, bool isUnicode, const char* value);
  void request(const std::string& path, Kind kind);
  void decodeTask();
  Work start(uint32_t generation);
  Work fail(uint32_t generation, const std::string& why);
  bool openDecoder(const std::string& path, AudioOutput* out);
  void closeDecoder();
  Produced produceTone();
  Produced produceDecoded();
  void noteRingFill(int rate);  // decode task: ringSteady_ once the ring holds kSteadyMs
  void noteStartProgress(int rate, bool full);  // decode task: fills in startTiming()
  static void boostTimerEntry(void* self);
  void boostTick();  // esp_timer task
  void applyDecodePriority(bool yielding, UiBoost::Why why, uint32_t ringMs, uint32_t nowMs);  // under boostLock_
  void runBench(const std::string& path);
  void setText(std::string& field, const std::string& value);

  std::unique_ptr<PcmRing> ring_;
  AudioShared shared_;
  TransportSync sync_;
  BtSink bt_;
  SpeakerSink speaker_;
  TaskHandle_t task_ = nullptr;

  // Owned by the decode task.
  fs::FS* fs_ = nullptr;
  std::unique_ptr<RingOutput> out_;
  std::unique_ptr<AudioFileSourceFS> file_;
  std::unique_ptr<AudioFileSourceID3> id3_;  // per MP3 track
  std::unique_ptr<AudioGeneratorMP3> mp3_;   // created fresh for each track
  std::unique_ptr<AudioGeneratorFLAC> flac_;
  AudioGenerator* decoder_ = nullptr;        // the one decoding now, or null
  const char* codec_ = "";
  bool toneTrack_ = false;
  bool clickTrack_ = false;                  // a tone: track made by click_, not tone_
  bool sourceDone_ = false;                  // decoder reached the end of the file
  bool described_ = false;
  ToneGen tone_;
  ClickGen click_;
  int16_t* chunk_ = nullptr;  // tone scratch buffer (PSRAM)

  mutable std::mutex lock_;  // guards request_ and the strings below
  Request request_;
  std::string description_;
  std::string title_;
  std::string artist_;
  std::string note_;

  std::atomic<uint32_t> trackStart_{0};  // ring readPos() where the current track begins
  std::atomic<uint64_t> busyUs_{0};      // decode task time spent producing, current track
  std::atomic<uint64_t> busyTotalUs_{0}; // the same, since boot
  std::atomic<bool> ringSteady_{false};  // see ringSteady()
  std::atomic<uint64_t> producedFrames_{0};

  // Counts pause() calls. The decode task un-pauses a newly started track
  // (start()) only if the player hasn't paused since asking for it: a Next
  // then a Pause must stay paused.
  std::atomic<uint32_t> pauses_{0};

  // Interaction boost (see setUiBoost()).
  std::mutex boostLock_;  // the priority change and boostEnabled_ together
  esp_timer_handle_t boostTimer_ = nullptr;
  UiBoost boost_;                               // esp_timer task (and setUiBoost, timer stopped)
  std::atomic<bool> boostEnabled_{false};
  std::atomic<bool> uiEverActive_{false};
  std::atomic<uint32_t> uiActiveMs_{0};
  std::atomic<bool> boostOn_{false};
  std::atomic<uint8_t> boostWhy_{0};
  std::atomic<uint32_t> boostChanges_{0};
  std::atomic<uint32_t> boostRingMs_{0};
  std::atomic<uint32_t> boostAtMs_{0};

  // Gentle refill (see setRefillPacing()).
  std::atomic<bool> paceEnabled_{false};
  std::atomic<uint32_t> paceCapX10_{15};

  // Start timing (see startTiming()): the request time is the loop's, the
  // rest the decode task's.
  std::atomic<uint32_t> requestMs_{0};
  std::atomic<uint32_t> startSeq_{0};
  std::atomic<int32_t> firstAudioMs_{-1};
  std::atomic<int32_t> ring500Ms_{-1};
  std::atomic<int32_t> steadyMs_{-1};
  std::atomic<int32_t> fullMs_{-1};
  uint32_t loggedStartSeq_ = 0;  // loop task

  // Loop task only.
  Output output_ = Output::Speaker;
  uint8_t volume_ = 30;  // the speaker's; Bluetooth's is in BtSink
  // The player's transport: playing since play()/resume(), until pause()/stop().
  // Unlike isPlaying() it doesn't blink between tracks or while the decode
  // task catches up, so the Bluetooth stream follows the player's intent.
  bool transportPlaying_ = false;
  Stats stats_{};
  uint32_t lastStatsMs_ = 0;
  uint32_t lastBtFrames_ = 0;
};
