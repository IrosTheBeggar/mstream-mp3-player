// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "ClickGen.h"
#include "GaplessEngine.h"
#include "GaplessJoin.h"
#include "LameTag.h"
#include "PcmRing.h"
#include "RingFeed.h"
#include "ToneGen.h"
#include "ToneTrack.h"
#include "TransportSync.h"
#include "RefillPacer.h"
#include "audio/AudioShared.h"
#include "audio/BtSink.h"
#include "audio/SpeakerSink.h"
#include "hal/IAudioBackend.h"

// Gapless playback on at boot (docs/GAPLESS.md); -DMSTREAM_GAPLESS=0 builds
// it off (the console's G1 turns it on).
#ifndef MSTREAM_GAPLESS
#define MSTREAM_GAPLESS 1
#endif

class AudioFileSourceFS;
class AudioGenerator;
class AudioGeneratorMP3;
class AudioOutput;
class GuardedSource;
class RingOutput;
class SeekableFlac;

// IAudioBackend for the Core2. A decode task turns the current track into PCM
// in a PSRAM ring (PcmRing), converted to 44.1 kHz on the way in (RingOutput;
// docs/RESAMPLER.md: 8-48 kHz, and 88.2/96 kHz at the 240 MHz CPU speed;
// any other rate fails the track, on both outputs); the active output,
// Bluetooth headphones or the internal speaker, plays from the ring. play() and stop() are requests: they
// post a new generation to TransportSync and wake the decode task, and
// finished()/failed() only ever describe the latest request.
//
// Tracks are .mp3/.flac files on the library filesystem, or the built-in test
// tones "tone:440", "tone:1000" and "tone:left" (440 Hz, left channel only),
// and 60 s click tracks with a known beat, "tone:click<bpm>" and
// "tone:click<bpm>off" (first beat 0.37 of a period in; see ClickGen), and
// an hour of digital silence, "tone:silence" (power measurements: the
// output runs at its full rate, nothing is heard). A tone or the silence
// can be made at another rate and converted like a file,
// "tone:1000@48000", "tone:silence@96000" (lib/core ToneTrack; the
// converter's test tracks).
//
// A play can start part of the way in (the resume point: play()'s
// `startMs`; lib/core TrackSeek, docs/ARCHITECTURE.md "Audio pipeline"): an
// MP3 at a byte from its Xing or VBRI table of contents or its bitrate, on
// a clean frame; a FLAC through libFLAC's own seek; a built-in track just
// counts from there. positionMs() and durationMs() count from the start, in
// 44.1 kHz ring frames whatever the track's rate.
//
// Gapless playback (docs/GAPLESS.md): the player names what follows
// (setNext()); at the end of a file the decode task opens it at once and
// writes on into the same ring (lib/core GaplessEngine), no discardAll(),
// no new generation, MP3s trimmed by their LAME tag (TrimFeed). The track
// heard (positionMs(), durationMs(), description()...) switches when the
// outputs read past the join (takeAdvance()), not when the decoder gets
// there. A word that changes while the next track is decoded ahead takes
// it back out of the ring (PcmRing::cutBack()) unless it has been heard.
class Core2AudioBackend : public IAudioBackend, private GaplessEngine::Tracks {
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
  bool play(const std::string& path, uint32_t durationHintMs, uint32_t startMs) override;
  void pause() override;
  void resume() override;
  void stop() override;
  void loop(uint32_t nowMs) override;
  bool isPlaying() const override;
  uint32_t positionMs() const override;
  // False while the decode task hasn't taken the last play() up (Pending):
  // until then positionMs() may still count the track before.
  bool positionKnown() const override;
  bool finished() const override;
  bool failed() const override;
  RateRefusal rateRefusal() const override;
  void setNext(const Next& next) override;
  bool takeAdvance(uint32_t* token) override;

  // Decodes up to 20 s of `path` as fast as possible, output discarded, and
  // prints how many times faster than realtime that was; a file at another
  // rate than 44.1 kHz is then decoded again through the converter, for
  // decode plus convert. Stops playback.
  void bench(const std::string& path);
  // The converter's bench (the console's Rb): 10 s of a fixed stereo signal
  // at each supported rate through RingOutput's own converter on the decode
  // task, output dropped; prints its cycles per second of audio and the
  // share of a core at the clock running now. Stops playback.
  void rateBench();
  // A test (the console's Rf<hz>, silent mode): plays `path` as if its rate
  // were `asHz`, so a 44.1 kHz MP3 is decoded and converted as a 48 kHz
  // one would be (the decoder's work per source frame and the
  // converter's): the load of a 48 kHz track where the card has none. The
  // pitch is wrong; nothing follows it.
  void playAsRate(const std::string& path, uint32_t asHz);
  // The CPU speed set at boot (PowerSettings::cpuBootMhz()): 88.2/96 kHz
  // tracks need 240 MHz. The setting, not the clock right now: the
  // console's Pc80 lowers that for quiet spells. 0 (until set): the clock.
  void setCpuMhz(uint16_t mhz) { cpuMhz_.store(mhz, std::memory_order_relaxed); }

  // The current track's conversion (the console's R; any task, a snapshot):
  // the source rate (0: not known yet), the route, ring frames per source
  // frame (num/den, exactly), and since the start or the last rate change
  // the source frames taken, the ring frames made and the samples clamped.
  // Once a track has ended, made == ceil(taken * num / den) exactly; until
  // then the filter holds back up to its delay's worth.
  struct RateStatus {
    uint32_t rate;
    const char* route;  // "147/160", "passthrough", "" (none yet)
    uint32_t num, den;
    uint32_t taken, made, clamped;
  };
  RateStatus rateStatus() const;
  // Where the converter's tables are read from (the console's R; any task):
  // the internal-RAM copy, or flash (none wanted, or no room for one), and
  // the copies that found no room since boot (logged only the first time:
  // a converted track that stutters after it shows here).
  struct TableStatus {
    bool inRam;
    uint32_t noRoom;
  };
  static TableStatus tableStatus();

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
  uint8_t speakerVolume() const { return volume_; }
  // The sleep timer's fade (FadeStage, after both outputs' gain; ENERGY.md
  // section 3): its target (Q15, at most 1.0), and back to 1.0 at once,
  // only while nothing is heard (a confirmed pause). Loop task. Never sent
  // to the headphones: their own level stays as it is.
  void setFade(uint16_t q15) { shared_.fade.setTarget(q15); }
  void restoreFade() { shared_.fade.restore(); }
  uint16_t fadeLevelQ15() const { return shared_.fade.levelQ15(); }
  uint16_t fadeTargetQ15() const { return shared_.fade.target(); }

  BtSink& bluetooth() { return bt_; }
  SpeakerSink& speaker() { return speaker_; }
  // Whether the outputs copy what they play to their taps (the beat
  // tracker's input): only while the Dance tab is up and tracking
  // (DanceMode switches them; ENERGY.md item 9). Off from boot.
  void setTapsOn(bool on) {
    bt_.setTapOn(on);
    speaker_.setTapOn(on);
  }
  bool tapsOn() const {
    const AudioTap* t = speaker_.tap() ? speaker_.tap() : bt_.tap();
    return t && t->enabled();
  }
  // What an output just played, for the beat tracker (nullptr: no PSRAM).
  const AudioTap* tap(Output output) const { return output == Output::Bluetooth ? bt_.tap() : speaker_.tap(); }
  // How long after an output's tap write its audio is heard, and how that
  // was found (for the logs): Bluetooth, the headphones' delay report plus
  // ~25 ms for ESP-IDF's queue and the radio; the speaker, its measured queue
  // plus the I2S DMA. Loop task.
  uint32_t outputLatencyUs(Output output, char* how, size_t howLen) const;
  // Sample rate of the audio in the ring: 44.1 kHz, every track converted.
  int sampleRate() const { return AudioShared::kRingRate; }
  const Stats& stats() const { return stats_; }  // refreshed by loop() once a second
  // Live numbers for the UI (any task; stats() is once a second):
  // audio waiting in the ring now, in ms. The
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
  // the UI gets what the decoder leaves. (Round 2 also tried the opposite, an
  // interaction boost that lowered the decoder below the loop while a list
  // moved: it measured worse, 7 fps against 13.9 and 200-400 ms stalls, and
  // was removed.)
  static constexpr UBaseType_t kDecodePriority = 2;

  // Gentle refill, on by default. At a track start or skip the decoder
  // fills the whole ~1.45 s ring flat out, which on core 1 starves the UI
  // for ~0.7 s (an MP3). With pacing on, during that fill (until the ring
  // is first full) and once the ring holds gentleFromMs (500 ms), the decode
  // task sleeps after each pass so that it produces at most capX10/10 times
  // realtime (RefillPacer; capX10 is at least 15, 1.5x); below 500 ms it
  // runs flat out as before, so the time to first audio doesn't change.
  // Refills later in the track (after a dip) are never paced. The scroll
  // lab's wp0 turns it off for A/B runs.
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
  // For the UI; set by the decode task: the heard track's (a track decoded
  // ahead has its own once its join is heard).
  std::string description() const;  // e.g. "MP3, 44100 Hz"
  std::string note() const;  // why the last track failed (or ended early), or ""
  // The heard track's length (ms), for the UI's progress: exact once its
  // file has ended (to the frame: where the next stream begins), and for
  // the built-in tracks; before, the file's header (an MP3's Xing/VBRI,
  // trimmed by its LAME tag; a FLAC's STREAMINFO) or an estimate from how
  // fast the decoder goes through it (lib/core TrackProgress: exact for a
  // constant-bitrate MP3, settling within seconds otherwise; a track
  // started part of the way in adds its start to the estimate of what is
  // left). 0: not known yet (the first ~1 s). Any task.
  uint32_t durationMs() const override;
  // Where the heard track started (ms into it; 0: its beginning, and every
  // joined track): a resume point, as it really landed (the last 5 s and
  // past the end start at 0). positionMs() includes it. Any task.
  uint32_t startOffsetMs() const { return book_.startMs(); }
  // durationMs() was read from the file, or is exact (the file has ended,
  // or a built-in track), not estimated.
  bool durationKnown() const;
  // Counts the tracks the outputs begin: every start (play()), and every
  // join heard (takeAdvance()). The sleep timer's EntryStart and the
  // Queue's learned lengths go by it: at a gapless advance the entry and
  // the count change together. Loop task.
  uint32_t trackSeq() const { return trackSeq_.load(std::memory_order_acquire); }

  // ---- gapless playback (the console's G) ----
  // Off (G0): no word is taken, a track decoded ahead is cut back out, and
  // from the next open no trimming, no header-frame skip, no guard bytes:
  // v0.5.0's ends, for the A/B (the player stops naming what follows too:
  // PlaybackController::setGapless()). Loop task; RAM only.
  void setGapless(bool on);
  bool gapless() const { return gapless_.load(std::memory_order_relaxed); }
  // Trimming by the LAME tag (Gt0/Gt1), from the next open: off, an MP3 is
  // played as decoded (the generator's lead still skipped).
  void setGaplessTrim(bool on) { gaplessTrim_.store(on, std::memory_order_relaxed); }
  bool gaplessTrim() const { return gaplessTrim_.load(std::memory_order_relaxed); }
  // G: the word, the boundary, the counters, the decoding track's trim.
  void printGapless() const;

private:
  enum class Produced : uint8_t { More, Done, Failed };
  enum class Kind : uint8_t { Play, Stop, Bench, RateBench };
  struct Request {
    std::string path;
    Kind kind = Kind::Stop;
    uint32_t pauses = 0;  // pauses_ when it was made
    uint32_t startMs = 0; // Play: this far in
    uint32_t hintMs = 0;  // Play: its length as known elsewhere (play()'s durationHintMs)
    uint32_t asHz = 0;    // Play: the rate it is converted from whatever it says (playAsRate(), a test)
  };

  // A track opened: what its file (or a built-in track's name) says before
  // any frame, and where its decoder begins (decode task).
  struct Prepared {
    std::string path;
    bool tone = false;
    ToneTrack toneSpec;
    bool mp3 = false;
    uint32_t rate = 0;        // what it says before its first frame (0: nothing)
    uint32_t from = 0;        // MP3: the byte the decoder begins at
    bool fromTop = true;      // ... the first audio frame (not a seek)
    uint32_t landedMs = 0;    // where it starts (a resume point)
    uint32_t knownMs = 0;     // its length, if it says (an MP3's trimmed)
    uint32_t flacSeekMs = 0;  // FLAC: libFLAC's seek after begin()
    uint32_t metadataBytes = 0;  // FLAC: its metadata blocks (a big picture is read through at the open)
    lametag::Info lame;
    lametag::Trim trim;
    bool guard = false;       // MP3: GuardedSource
  };

  static void taskEntry(void* self);
  void request(const std::string& path, Kind kind, uint32_t startMs = 0, uint32_t hintMs = 0, uint32_t asHz = 0);
  void decodeTask();
  void start(uint32_t generation);
  void fail(uint32_t generation, const std::string& why);
  // fail() for a rate the converter refused, kept for rateRefusal().
  void failRate(uint32_t generation);
  // The engine's phase as TransportSync's (Decoding, Draining, Ended) and
  // the outputs' flags, after a step.
  void reportPhase(uint32_t generation);
  // Sleeps up to `ms`, woken early by play()/stop()/setNext()
  // (xTaskNotifyGive): a cut or a new request never waits a whole pause.
  void rest(uint32_t ms);
  RingFeed& feed();
  uint32_t cpuMhz() const;
  // Why the track's rate was refused: "37800 Hz isn't supported (...)".
  std::string refusalText();
  // Opens `path` (a file into file_, or a built-in track's name) and reads
  // what it says: its rate, length, LAME tag, the byte to begin at.
  // `startMs` > 0: part of the way in; `hintMs` its length as known
  // elsewhere (0: none). Nothing is fed. False: it can't be played.
  bool prepare(const std::string& path, uint32_t startMs, uint32_t hintMs, Prepared* p);
  // The prepared track's decoder begins into `out` (its trim armed when
  // `trimmed`: RingOutput's own), the per-track counters start again.
  bool beginPrepared(const Prepared& p, AudioOutput* out, bool trimmed);
  // prepare() + beginPrepared() (a start, the bench).
  bool openDecoder(const std::string& path, AudioOutput* out, uint32_t startMs, uint32_t hintMs, bool trimmed);
  // An MP3 at `startMs`, a track `known` ms long (0: not said): the byte to
  // hand the decoder from (a frame's), and where that lands (`landedMs`),
  // found through `probe` (PSRAM, holding `got` bytes from `audioStart`,
  // the end of the tags; reused for the frame search). 0: from the top
  // (the last 5 s, past the end, a VBR file with nothing to place it by,
  // no clean frame there, or too little after it); `landedMs` is then left
  // alone.
  uint32_t mp3StartByte(uint8_t* probe, uint32_t got, uint32_t audioStart, uint32_t startMs, uint32_t hintMs,
                        uint32_t known, uint32_t* landedMs);
  void closeDecoder();
  Produced produceTone();
  Produced produceDecoded();
  // An early end of the decoding track (a rate refused mid-stream): why,
  // for note() when it is heard.
  void endedEarly(const std::string& why);
  // GaplessEngine::Tracks (the decode task's side of a join).
  bool probe(const GaplessJoin::Offer& next, uint32_t* rate) override;
  bool start() override;
  void close() override;
  void note(const GaplessEngine::Note& n) override;
  void noteRingFill();  // decode task: ringSteady_ once the ring holds kSteadyMs
  void noteStartProgress(bool full);  // decode task: fills in startTiming()
  void publishRate();   // decode task: rateStatus()'s snapshot
  void runBench(const std::string& path);
  void runRateBench();
  // The converter's fast kernel (MAC16) against the C kernel, bit for bit
  // (RateConverter::kernelSelfTest()): on at boot only if they agree, and
  // never again after a mismatch. Logs the result; true: the fast kernel.
  bool checkKernel(const char* when);
  // Rb: every route through RingOutput's converter with each kernel, the
  // same input in blocks of varying size; the outputs must be identical.
  bool checkRoutes();
  // Rb: cycles per source frame through RingOutput::ConsumeSample(), the
  // generators' path, one frame at a time.
  void benchConsume(uint32_t hz);
  // Rb: cycles per dot product (48 taps, one channel) for each kernel, the
  // row in flash (walked through the table, or one row) or in internal RAM.
  void benchKernel();
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
  bool kernelFailed_ = false;  // the fast kernel failed a self-test: the C kernel until a restart
  std::unique_ptr<AudioFileSourceFS> file_;
  std::unique_ptr<GuardedSource> guard_;     // file_ with 8 zero bytes after it (MP3)
  std::unique_ptr<AudioGeneratorMP3> mp3_;   // created fresh for each track
  std::unique_ptr<SeekableFlac> flac_;
  AudioGenerator* decoder_ = nullptr;        // the one decoding now, or null
  const char* codec_ = "";
  bool toneTrack_ = false;
  bool clickTrack_ = false;                  // a tone: track made by click_, not tone_
  bool early_ = false;                        // the decoding track ended early (an error)
  uint32_t toneN_ = 0;                       // frames in chunk_ (a built-in track's)
  uint32_t toneAt_ = 0;                      // of which RingOutput has taken this many
  bool described_ = false;
  ToneGen tone_;
  ClickGen click_;
  int16_t* chunk_ = nullptr;  // tone scratch buffer (PSRAM)
  std::atomic<uint16_t> cpuMhz_{0};  // setCpuMhz()
  // Gapless playback: the boundary book (any task), the decode task's
  // state machine, the track prepared for a join, the PSRAM it uses (two
  // feed marks ~2 KB each, the trim's 16 KB hold).
  GaplessJoin book_;
  std::unique_ptr<GaplessEngine> engine_;
  Prepared prepared_;
  RingFeed::Mark* marks_[2] = {nullptr, nullptr};
  int16_t* holdBuf_ = nullptr;
  Phase reported_ = Phase::Idle;  // what reportPhase() said last (decode task)
  std::atomic<bool> gapless_{MSTREAM_GAPLESS != 0};
  std::atomic<bool> gaplessTrim_{true};
  int64_t probeUs_ = 0;           // decode task: the join's open began
  uint32_t openMs_ = 0;           // ... and took (the log)
  std::atomic<int64_t> nextAtUs_{0};      // the last setNext() (edit-to-cut, for G)
  std::atomic<uint32_t> cutLatencyUs_{0}; // the last cut's, from the word that asked for it
  std::atomic<uint32_t> cutLatencyMaxUs_{0};
  std::atomic<uint32_t> advances_{0};     // joins heard (G)

  // rateStatus()'s snapshot (decode task -> any).
  std::atomic<uint32_t> convRate_{0};
  std::atomic<const char*> convRoute_{""};
  std::atomic<uint32_t> convNum_{1};
  std::atomic<uint32_t> convDen_{1};
  std::atomic<uint32_t> convTaken_{0};
  std::atomic<uint32_t> convMade_{0};
  std::atomic<uint32_t> convClamped_{0};
  // rateRefusal(): the refused rate (0: none) and whether a setting would
  // take it. Written before the Failed report, cleared at every start.
  std::atomic<uint32_t> refusedHz_{0};
  std::atomic<bool> refusedForCpu_{false};

  mutable std::mutex lock_;  // guards request_ and the strings below
  Request request_;
  std::string description_;       // the decoding track's
  std::string heardDescription_;  // the heard track's while another decodes after it (heardOverride_)
  bool heardOverride_ = false;
  std::string note_;              // the heard track's
  std::string aheadNote_;         // a track decoded ahead's, for when it is heard

  std::atomic<uint64_t> busyUs_{0};      // decode task time spent producing, current track
  std::atomic<uint64_t> busyTotalUs_{0}; // the same, since boot
  std::atomic<bool> ringSteady_{false};  // see ringSteady()
  std::atomic<uint64_t> producedFrames_{0};  // ring frames (44.1 kHz) of the current track
  // For durationMs(): the file's position when the first audio came and
  // now, its size (decode task; 0 for a tone), or a tone's known length.
  std::atomic<uint32_t> srcPos0_{0};
  std::atomic<uint32_t> srcPos_{0};
  std::atomic<uint32_t> srcSize_{0};
  std::atomic<uint32_t> knownDurationMs_{0};
  // Where the decoding track started, ms into it (the heard one's is the
  // book's: startOffsetMs()).
  std::atomic<uint32_t> startMs_{0};
  std::atomic<uint32_t> trackSeq_{0};  // trackSeq(): the decode task's starts, the loop's advances
  // The decoding track's trim, for G (decode task -> any).
  static constexpr uint32_t kTrimMp3 = 1, kTrimLame = 2, kTrimCrcBad = 4;
  std::atomic<uint32_t> trimNow_{0};
  std::atomic<uint32_t> trimDelay_{0}, trimPadding_{0}, trimSkip_{0}, trimHold_{0};

  // Counts pause() calls. The decode task un-pauses a newly started track
  // (start()) only if the player hasn't paused since asking for it: a Next
  // then a Pause must stay paused.
  std::atomic<uint32_t> pauses_{0};

  // Gentle refill (see setRefillPacing()).
  std::atomic<bool> paceEnabled_{true};
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
  uint32_t loggedAmpSwitches_ = 0;  // loop task

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
