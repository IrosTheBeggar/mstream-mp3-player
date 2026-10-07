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
#include "DecoderArena.h"
#include "GaplessEngine.h"
#include "GaplessJoin.h"
#include "LameTag.h"
#include "OggOpus.h"
#include "OpusOpenCache.h"
#include "PcmRing.h"
#include "RingFeed.h"
#include "SeekIndex.h"
#include "ToneGen.h"
#include "ToneTrack.h"
#include "TrackSeek.h"
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
class AudioOutput;
class GuardedSource;
class OpusGenerator;
class PinnedMp3;
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
// Tracks are .mp3, .flac and .opus files on the library filesystem (Ogg
// Opus through OpusGenerator and the lib/core reader, docs/OPUS.md: always
// 48 kHz, so it goes through the converter's block path; the generator
// trims it itself, and a start part of the way in is the reader's plan,
// exact, section 9), or the built-in test
// tones "tone:440", "tone:1000" and "tone:left" (440 Hz, left channel only),
// and 60 s click tracks with a known beat, "tone:click<bpm>" and
// "tone:click<bpm>off" (first beat 0.37 of a period in; see ClickGen), and
// an hour of digital silence, "tone:silence" (power measurements: the
// output runs at its full rate, nothing is heard). A tone or the silence
// can be made at another rate and converted like a file,
// "tone:1000@48000", "tone:silence@96000" (lib/core ToneTrack; the
// converter's test tracks).
//
// A play can start part of the way in (the resume point, a seek: play()'s
// StartAt; docs/SEEK.md): an MP3 by a start plan (lib/core TrackSeek: a
// preroll frame handed to the decoder, the landing frame and the samples
// skipped from it, TrimFeed::armAt() dropping everything before), from the
// first source that gives one: the resume point's anchor, checked against
// the file; the run index of what was decoded (lib/core SeekIndex: a seek
// back into this run); CBR arithmetic; LAME's TOC inverted; another TOC or
// the average bitrate, then a chain of frame headers. A FLAC through
// libFLAC's own seek, by sample; an Opus track by the reader's plan
// (lib/core OggOpus: a bisection on the pages' granule positions to the
// last page before the target less a preroll, 200 ms for a seek and 600 ms
// for a resume anchor, the packets before it skipped undecoded, the
// samples before the target dropped: exact, planOpus(); docs/OPUS.md
// section 9); a built-in track just counts from there.
// positionMs() and durationMs() count from the start, in 44.1 kHz ring
// frames whatever the track's rate. While a track decodes, the run index
// records every 4th MP3 frame (two 24 KB PSRAM slots: the heard track and
// the one decoded ahead; a FLAC or Opus run is its header alone), and
// resumeAnchor() gives a pause's anchor from it: the next start picks up
// on that very sample.
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
    uint32_t maxPassUs;        // the longest decode pass of the heard track (docs/OPUS.md gate G6; a track decoded
                               // ahead has its own once its join is heard)
  };

  Core2AudioBackend();
  ~Core2AudioBackend() override;  // out of line: the decoder types are incomplete here

  // Allocates the ring, starts Bluetooth (headphones named `btSinkName`, see
  // BtSink), the speaker pump and the decode task. `fs` is the library
  // (nullptr: tones only); `stateDir` the player's own folder on it
  // ("/.player"; nullptr: none), where the Opus open cache is kept
  // (opus.idx: docs/OPUS.md section 10).
  bool begin(fs::FS* fs, const char* stateDir, const char* btSinkName);

  // IAudioBackend
  bool play(const std::string& path, uint32_t durationHintMs, uint32_t startMs) override;
  bool play(const std::string& path, const StartAt& at) override;
  // The heard track's anchor at the read position (loop task): with gapless
  // trimming on (G1 and Gt1), for a file whose run is the request's now.
  bool resumeAnchor(ResumeAnchor* out) const override;
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
  // (IAudioBackend::seekable() stays the base's: every format starts part
  // of the way in, an .opus since docs/OPUS.md section 9.)
  RateRefusal rateRefusal() const override;
  // While failed(): the refusal in a few words (an Opus file refused at
  // its open: OpusGenerator::refusalNote()); "" for the rest.
  size_t failureNote(char* buf, size_t size) const override;
  void setNext(const Next& next) override;
  bool takeAdvance(uint32_t* token) override;

  // Decodes up to 20 s of `path` as fast as possible, output discarded, and
  // prints how many times faster than realtime that was; a file at another
  // rate than 44.1 kHz is then decoded again through the converter, for
  // decode plus convert. Stops playback.
  void bench(const std::string& path);
  // Where an Opus track's decoder state goes from its next open (the
  // console's O knob; docs/OPUS.md gate G1's A/B): 0 the pinned block (PSRAM,
  // its lower 2 MB; the default), 1 internal RAM, 2 PSRAM above 0x3FA00000.
  // Any task; the decode task reads it at the open.
  void setOpusPlacement(uint8_t where) { opusPlacement_.store(where, std::memory_order_relaxed); }
  uint8_t opusPlacement() const { return opusPlacement_.load(std::memory_order_relaxed); }
  // Where the converter's table copy goes from the next converted track
  // (the console's Ot1 / Ot0; docs/OPUS.md gate G5, section 8.11): the
  // PSRAM block pinned at boot next to the decoder arena's (the default
  // since M2's device check measured it; the .cpp's tablesApply() has the
  // figures), or internal RAM (Ot0). Any task. A copy already made in the
  // other place is dropped at the next request's start.
  void setOpusTablesPinned(bool pinned);
  bool opusTablesPinned() const;
  // The block, for the logs ("at 0x3f8..., PSRAM, its lower 2 MB"); false: none.
  bool opusTablesBlock(char* buf, size_t size) const;
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
  // the internal-RAM copy, the pinned PSRAM block's (Ot1), or flash (none
  // wanted, or no room for one), and the copies that found no room since
  // boot (logged only the first time: a converted track that stutters
  // after it shows here).
  struct TableStatus {
    bool inRam;
    bool inPsram;
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
    ResumeAnchor anchor;  // Play: the resume point's (kind None: none)
  };

  // A track opened: what its file (or a built-in track's name) says before
  // any frame, and where its decoder begins (decode task).
  struct Prepared {
    std::string path;
    bool tone = false;
    ToneTrack toneSpec;
    bool mp3 = false;
    bool opus = false;        // an Ogg Opus file (neither: a FLAC); opened in opus_, its length known
    uint32_t rate = 0;        // what it says before its first frame (0: nothing)
    uint32_t from = 0;        // MP3: the byte the decoder begins at
    bool fromTop = true;      // ... the first audio frame (not a seek)
    uint32_t landedMs = 0;    // where it starts (a resume point)
    uint32_t knownMs = 0;     // its length, if it says (an MP3's trimmed)
    bool flacSeek = false;    // FLAC: libFLAC's seek after begin(), to:
    uint64_t flacSample = 0;
    uint32_t metadataBytes = 0;  // FLAC: its metadata blocks (a big picture is read through at the open)
    lametag::Info lame;
    lametag::Trim trim;
    bool guard = false;       // MP3: GuardedSource
    // A start part of the way in (MP3: by its plan, TrimFeed::armAt()).
    bool planned = false;
    trackseek::Plan plan;
    // Opus: the reader's plan (planOpus(); OpusGenerator::setStartPlan()):
    // the page reading starts at, where decoding and keeping begin.
    bool opusPlanned = false;
    oggopus::StartPlan opusPlan;
    // The run index's header for this track (SeekIndex::Run): its file, the
    // first audio frame and its hash, the timeline's offset there
    // (-(delay + 529) with LAME's tag), a FLAC's or an Opus track's rate
    // and total samples (STREAMINFO's; the Opus tail scan's exact length,
    // 0 when no last page was found).
    uint32_t pathHash = 0;
    uint32_t fileSize = 0;
    uint32_t firstAudio = 0;
    uint32_t firstHash = 0;
    int32_t topT0 = 0;
    uint64_t totalSamples = 0;
    uint64_t startSample = 0;  // the run's base: where it starts on the timeline
    bool startExact = true;
  };

  static void taskEntry(void* self);
  void request(const std::string& path, Kind kind, uint32_t startMs = 0, uint32_t hintMs = 0, uint32_t asHz = 0,
               const ResumeAnchor& anchor = ResumeAnchor{});
  void decodeTask();
  void start(uint32_t generation);
  // `note`: the same in a few words for Now Playing (failureNote()), when
  // there is a better one than "can't play it".
  void fail(uint32_t generation, const std::string& why, const char* note = "");
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
  // what it says: its rate, length, LAME tag, where to begin. `at.ms` > 0
  // (or an anchor): part of the way in; `at.hintMs` its length as known
  // elsewhere (0: none). Nothing is fed. False: it can't be played.
  bool prepare(const std::string& path, const StartAt& at, Prepared* p);
  // The prepared track's decoder begins into `out` (its trim armed when
  // `trimmed`: RingOutput's own), the per-track counters start again.
  bool beginPrepared(const Prepared& p, AudioOutput* out, bool trimmed);
  // prepare() + beginPrepared() (a start, the bench).
  bool openDecoder(const std::string& path, AudioOutput* out, uint32_t startMs, uint32_t hintMs, bool trimmed);
  // An MP3 started part of the way in: its plan (p->plan, p->planned),
  // from `probe` (PSRAM, holding `got` bytes from `audioStart`, the end of
  // the tags): the anchor, the run index, then trackseek::plan(), reading
  // through `scratch` (kScratchBytes, PSRAM). Not planned: from the top
  // (logged why).
  void planMp3(const uint8_t* probe, uint32_t got, uint32_t audioStart, const StartAt& at, uint8_t* scratch,
               Prepared* p);
  // An Opus track started part of the way in: its plan (p->opusPlan,
  // p->opusPlanned) from the generator's reader after the open (docs/OPUS.md
  // section 9): the resume anchor, checked against the file (its size, the
  // exact length, the tail rule) and planned with the 600 ms resume preroll;
  // else the millisecond, the tail rule first, with the 200 ms seek preroll.
  // Both land on the exact sample. Not planned: from the top (logged why).
  void planOpus(OpusGenerator* g, const StartAt& at, Prepared* p);
  // The decoding track's run begins in the index (decode task).
  void beginRun(const Prepared& p);
  // After a pass: the landing settled (the run's base), and the frame the
  // pass ended on recorded (decode task).
  void noteRun();
  void closeDecoder();
  // A new MP3 generator for the next track, the one before destroyed first
  // (it gives the state block back): on the pinned block, or, without it,
  // ESP8266Audio's own per-track malloc (logged). Null: no RAM.
  PinnedMp3* makeMp3();
  // Where libmad's state is, for the logs: "at 0x3f8..., PSRAM, its lower 2 MB".
  void describeMp3State(char* buf, size_t size) const;
  // The Opus generator, made at the first .opus and kept (its PSRAM buffers
  // with it; docs/OPUS.md). Null: no RAM.
  OpusGenerator* opusGenerator();
  // The Opus open cache (OpusOpenCache, docs/OPUS.md section 10): what an
  // open learnt about a file, by its path and size, so the next open of
  // it (a seek on the playing track, a track played before, the resume
  // point at a boot) is one read. The decode task finds and puts (under
  // opusCacheLock_); the loop task loads it at begin() and saves it a few
  // seconds after a change (saveOpusCache(): written aside, then swapped
  // in, as the library's cache is; a write that fails is tried again
  // kOpusCacheRetryMs later), to opusCachePath_.
  bool findOpusRecord(const std::string& path, uint32_t fileSize, oggopus::OpenRecord* out);
  void putOpusRecord(const std::string& path, const oggopus::OpenRecord& rec);
  void forgetOpusRecord(const std::string& path, uint32_t fileSize);
  void loadOpusCache();
  void saveOpusCache();
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
  std::unique_ptr<PinnedMp3> mp3_;           // created fresh for each track (makeMp3()); stopped: holds no state
  // libmad's frame and synth state (25 KB), or the Opus decoder's state and
  // its frame's PCM (38 KB, layout opusLayout_): one PSRAM block from boot,
  // in the window's fast lower 2 MB, lent to one generator at a time
  // (src/audio/PinnedMp3.h, src/audio/OpusGenerator.h; docs/RESAMPLER.md
  // section 10d, docs/OPUS.md).
  DecoderArena mp3Arena_;
  int opusLayout_ = -1;
  bool mp3Pinned_ = false;    // mp3_ decodes on it
  uint32_t mp3Unpinned_ = 0;  // MP3 tracks decoded without it (no block, or lent out)
  std::unique_ptr<SeekableFlac> flac_;
  std::unique_ptr<OpusGenerator> opus_;      // made at the first .opus, kept (opusGenerator())
  std::atomic<uint8_t> opusPlacement_{0};    // setOpusPlacement()
  // The Opus open cache (findOpusRecord() etc.): its entries in PSRAM, the
  // lock both tasks take, the file it is saved to (empty: not saved), when
  // its save is due (millis(); kOpusCacheSaveDelayMs after the last
  // change, so a run of opens saves once; kOpusCacheRetryMs after a write
  // that failed, the cache marked dirty again: the queue's saver's wait)
  // and the save counts.
  static constexpr uint32_t kOpusCacheSaveDelayMs = 3000;
  static constexpr uint32_t kOpusCacheRetryMs = 10000;
  OpusOpenCache opusCache_;
  std::mutex opusCacheLock_;
  std::string opusCachePath_;
  uint32_t opusCacheDueMs_ = 0;  // under opusCacheLock_
  uint32_t opusCacheCheckMs_ = 0;  // loop task: the last look at it
  uint32_t opusCacheSaves_ = 0;
  uint32_t opusCacheSaveFails_ = 0;
  AudioGenerator* decoder_ = nullptr;        // the one decoding now, or null
  const char* codec_ = "";
  std::string prepareWhy_;                   // decode task: why prepare() refused the file ("" : no reason given)
  std::string prepareNote_;                  // ... in a few words for the screen ("": none)
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
  // The run index (docs/SEEK.md section 4.3): two slots of
  // SeekIndex::kCapacity entries in PSRAM (24 KB each). On the decode task:
  // the request's generation and the decoding MP3's recorder.
  SeekIndex index_;
  SeekIndex::Entry* indexSlots_[2] = {nullptr, nullptr};
  uint32_t runGen_ = 0;
  SeekRecorder recorder_;
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
  std::string failNote_;          // why the last request failed, in a few words (failureNote())

  std::atomic<uint64_t> busyUs_{0};      // decode task time spent producing, current track
  std::atomic<uint64_t> busyTotalUs_{0}; // the same, since boot
  std::atomic<uint32_t> maxPassUs_{0};       // the longest pass, the decoding track's (reset at each begin)
  std::atomic<uint32_t> heardMaxPassUs_{0};  // ... the heard track's (Stats::maxPassUs): the same while that is
                                             // the track decoding, frozen while the next decodes ahead, the joined
                                             // track's once its join is heard (takeAdvance())
  std::atomic<uint32_t> heardToken_{0};      // the heard track's token (0: the request's own; GaplessJoin)
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
