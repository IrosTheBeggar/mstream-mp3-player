// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "audio/Core2AudioBackend.h"

#include <Arduino.h>
#include <AudioFileSourceFS.h>
#include <AudioFileSourceID3.h>
#include <AudioGeneratorFLAC.h>
#include <AudioGeneratorMP3.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "TrackProgress.h"
#include "TrackSeek.h"
#include "audio/RingOutput.h"

namespace {
constexpr uint32_t kRingFrames = 65536;  // ~1.5 s at 44.1 kHz, 256 KB of PSRAM
constexpr uint32_t kChunkFrames = 1024;  // produced per pass of the decode task
constexpr uint32_t kDecodeStack = 16384;
constexpr uint32_t kToneRate = 44100;
constexpr uint32_t kToneSeconds = 30;
constexpr uint32_t kClickSeconds = 60;
constexpr uint32_t kSilenceSeconds = 3600;  // "tone:silence", for power measurements
// Bluetooth: what the headphones report plus ESP-IDF's frame queue and the
// air (an estimate); without a report, what the Powerbeats Pro report.
constexpr uint32_t kBtExtraUs = 25000;
constexpr uint32_t kBtDefaultReportUs = 150000;
// The speaker before its first buffer has been timed: ~3 buffers + DMA.
constexpr uint32_t kSpeakerDefaultUs = 115000;
constexpr uint32_t kBenchSeconds = 20;
// An MP3's first bytes after its tags (its Xing/VBRI header, PSRAM), and
// after a seek a frame and the next one's header (a frame is at most 1,441
// bytes).
constexpr uint32_t kMp3Probe = 4096;

// "1:23" (m:ss).
void mmss(uint32_t ms, char* buf, size_t size) {
  const uint32_t s = ms / 1000;
  snprintf(buf, size, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

std::string extensionOf(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  std::string ext = dot == std::string::npos ? "" : path.substr(dot);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext;
}

// Swallows decoded audio and counts it, for bench(). Refuses a sample every
// kBurst frames so the generator's loop() returns and the time can be checked.
class CountingOutput : public AudioOutput {
public:
  bool begin() override { return true; }
  bool SetRate(int hz) override {
    rate = hz;
    return true;
  }
  bool SetChannels(int) override { return true; }
  bool ConsumeSample(int16_t[2]) override {
    if (budget == 0) return false;
    --budget;
    ++frames;
    return true;
  }
  bool stop() override { return true; }

  static constexpr uint32_t kBurst = 4096;
  int rate = 0;
  uint32_t budget = 0;
  uint64_t frames = 0;
};
}  // namespace

// libFLAC's seek (its SEEKTABLE when the file has one, else a bisection on
// the frames' headers), which AudioGeneratorFLAC doesn't expose: its decoder
// is a protected member.
class SeekableFlac : public AudioGeneratorFLAC {
public:
  // After begin(): to `sample`. The seek reads the metadata, then decodes
  // the frame the sample is in and hands it over from that sample (write_cb
  // keeps it for loop()). loop() learns the stream's format only after a
  // frame of its own, so it is set here: otherwise that first frame would
  // be read as 8-bit. (A rate the output refuses, 48 kHz on Bluetooth,
  // fails the track at its first loop(), as it would from the top.) False:
  // not there; the decoder is then in its seek error state: start again.
  bool seekTo(uint64_t sample) {
    if (!flac || !FLAC__stream_decoder_seek_absolute(flac, sample)) return false;
    sampleRate = FLAC__stream_decoder_get_sample_rate(flac);
    channels = static_cast<uint16_t>(FLAC__stream_decoder_get_channels(flac));
    bitsPerSample = static_cast<uint16_t>(FLAC__stream_decoder_get_bits_per_sample(flac));
    if (sampleRate == 0 || channels == 0) return false;
    output->SetRate(static_cast<int>(sampleRate));
    output->SetChannels(channels);
    return true;
  }
};

Core2AudioBackend::Core2AudioBackend() = default;
Core2AudioBackend::~Core2AudioBackend() = default;

bool Core2AudioBackend::begin(fs::FS* fs, const char* btSinkName) {
  auto* ringBuffer = static_cast<int16_t*>(
      heap_caps_malloc(kRingFrames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  chunk_ = static_cast<int16_t*>(
      heap_caps_malloc(kChunkFrames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  if (!ringBuffer || !chunk_) return false;
  ring_.reset(new PcmRing(ringBuffer, kRingFrames));

  fs_ = fs;
  out_.reset(new RingOutput(*ring_, shared_));
  if (fs_) file_.reset(new AudioFileSourceFS(*fs_));

  bt_.begin(*ring_, shared_, btSinkName);
  speaker_.begin(*ring_, shared_);
  setOutput(Output::Speaker);
  setVolume(volume_);

  // Internal-RAM stack: flash reads (LittleFS) can't run on a PSRAM stack.
  xTaskCreatePinnedToCore(taskEntry, "decode", kDecodeStack, this, kDecodePriority, &task_, APP_CPU_NUM);
  return task_ != nullptr;
}

// ---- control side (loop task) ----

void Core2AudioBackend::request(const std::string& path, Kind kind, uint32_t startMs, uint32_t hintMs) {
  if (!task_) return;  // begin() failed
  {
    std::lock_guard<std::mutex> guard(lock_);
    request_ = {path, kind, pauses_.load(), startMs, hintMs};
  }
  requestMs_.store(millis(), std::memory_order_relaxed);
  sync_.post(kind == Kind::Play ? Phase::Pending : Phase::Idle);
  xTaskNotifyGive(task_);
}

bool Core2AudioBackend::play(const std::string& path, uint32_t durationHintMs, uint32_t startMs) {
  // Un-paused by the decode task once the old track is discarded (start()), so
  // a paused ring never plays a burst of the previous track first.
  transportPlaying_ = true;
  request(path, Kind::Play, startMs, durationHintMs);
  return true;
}

void Core2AudioBackend::stop() {
  transportPlaying_ = false;
  request("", Kind::Stop);
}

void Core2AudioBackend::bench(const std::string& path) {
  transportPlaying_ = false;
  request(path, Kind::Bench);
}

void Core2AudioBackend::pause() {
  pauses_.fetch_add(1);  // before paused: see start()
  shared_.paused = true;
  transportPlaying_ = false;
}

void Core2AudioBackend::resume() {
  shared_.paused = false;
  transportPlaying_ = true;
}

bool Core2AudioBackend::isPlaying() const {
  const Phase p = sync_.phase();
  return !shared_.paused &&
         (p == Phase::Pending || p == Phase::Decoding || p == Phase::Draining);
}

uint32_t Core2AudioBackend::durationMs() const {
  const uint32_t known = knownDurationMs_.load(std::memory_order_relaxed);
  if (known) return known;
  // The estimate is of what is left from where the decoder began (the
  // first audio's file position): a start part of the way in adds its time.
  const uint32_t left =
      progress::estimateDurationMs(producedFrames_.load(std::memory_order_relaxed), shared_.rate,
                                   srcPos0_.load(std::memory_order_relaxed), srcPos_.load(std::memory_order_relaxed),
                                   srcSize_.load(std::memory_order_relaxed));
  return left ? startMs_.load(std::memory_order_relaxed) + left : 0;
}

uint32_t Core2AudioBackend::positionMs() const {
  const int rate = shared_.rate;
  const uint32_t frames = ring_->readPos() - trackStart_;
  const uint32_t played = rate > 0 ? static_cast<uint32_t>(static_cast<uint64_t>(frames) * 1000 / rate) : 0;
  return startMs_.load(std::memory_order_relaxed) + played;
}

bool Core2AudioBackend::positionKnown() const { return sync_.phase() != Phase::Pending; }

bool Core2AudioBackend::finished() const { return sync_.phase() == Phase::Ended; }
bool Core2AudioBackend::failed() const { return sync_.phase() == Phase::Failed; }

void Core2AudioBackend::setOutput(Output output) {
  output_ = output;
  ring_->setConsumer(output == Output::Bluetooth ? BtSink::kConsumerId
                                                 : SpeakerSink::kConsumerId);
  Serial.printf("[audio] output: %s\n", output == Output::Bluetooth ? "bluetooth" : "speaker");
}

void Core2AudioBackend::setVolume(uint8_t percent) {
  if (percent > 100) percent = 100;
  if (output_ == Output::Bluetooth) {
    bt_.setVolume(percent);
    return;
  }
  volume_ = percent;
  speaker_.setVolume(volume_);
}

void Core2AudioBackend::stepVolume(int delta) {
  if (output_ == Output::Bluetooth) {
    bt_.stepVolume(delta);
    return;
  }
  volume_ = static_cast<uint8_t>(std::min(100, std::max(0, volume_ + delta)));
  speaker_.setVolume(volume_);
}

void Core2AudioBackend::setSpeakerVolume(uint8_t percent) {
  volume_ = percent > 100 ? 100 : percent;
  speaker_.setVolume(volume_);
}

uint8_t Core2AudioBackend::volume() const {
  return output_ == Output::Bluetooth ? bt_.volume() : volume_;
}

uint32_t Core2AudioBackend::outputLatencyUs(Output output, char* how, size_t howLen) const {
  if (output == Output::Bluetooth) {
    const uint32_t report = bt_.delayReportUs();
    const uint32_t used = report ? report : kBtDefaultReportUs;
    snprintf(how, howLen, "bt: %s %.1f + %lu ms", report ? "report" : "no report, assumed", used / 1000.0f,
             (unsigned long)(kBtExtraUs / 1000));
    return used + kBtExtraUs;
  }
  const uint32_t queue = speaker_.queueLatencyUs();
  if (queue == 0) {
    snprintf(how, howLen, "speaker: not measured yet, assumed");
    return kSpeakerDefaultUs;
  }
  snprintf(how, howLen, "speaker: queue %.1f + dma %.1f ms", queue / 1000.0f, speaker_.dmaLatencyUs() / 1000.0f);
  return queue + speaker_.dmaLatencyUs();
}

void Core2AudioBackend::loop(uint32_t nowMs) {
  // Every pass: the Bluetooth media stream runs while the player plays on it,
  // and is suspended 3 s after that stops.
  bt_.update(nowMs, output_ == Output::Bluetooth && transportPlaying_);
  // The speaker amp's switches (the pump makes them: AmpGate).
  const uint32_t switches = speaker_.ampSwitches();
  if (switches != loggedAmpSwitches_) {
    loggedAmpSwitches_ = switches;
    const bool on = SpeakerSink::ampOn();
    const AmpGate::Why why = speaker_.ampWhy();
    Serial.printf("[speaker] amp %s (%s)\n", on ? "on" : "off: I2S stopped, AXP192 GPIO2 low",
                  why == AmpGate::Why::Asked   ? "asked: Pa"
                  : why == AmpGate::Why::Quiet ? "quiet for 2 s"
                                               : "audio to play");
  }
  // How the last start filled the ring, once it's full (or 5 s on).
  // Signed: nowMs was read at the top of the main loop, before the console
  // or a touch made the request, so it can be a few ms before requestMs_
  // (unsigned, that wrapped and logged every start at once, all -1).
  const uint32_t seq = startSeq_.load(std::memory_order_acquire);
  if (seq != loggedStartSeq_ &&
      (fullMs_.load(std::memory_order_relaxed) >= 0 ||
       static_cast<int32_t>(nowMs - requestMs_.load(std::memory_order_relaxed)) > 5000)) {
    loggedStartSeq_ = seq;
    const StartTiming t = startTiming();
    const RefillPacer::Config pace = refillPacing();
    char paced[40] = "off";
    if (pace.enabled) {
      snprintf(paced, sizeof(paced), "%.1fx from %lu ms", pace.capX10 / 10.0f, (unsigned long)pace.gentleFromMs);
    }
    Serial.printf("[audio] refill: first audio in the ring %ld ms after the request, 500 ms buffered at %ld ms, "
                  "%lu ms (steady) at %ld ms, full at %ld ms (-1: not reached); pacing %s\n",
                  (long)t.firstAudioMs, (long)t.ring500Ms, (unsigned long)kSteadyMs, (long)t.steadyMs, (long)t.fullMs,
                  paced);
  }
  if (nowMs - lastStatsMs_ < 1000) return;
  const uint32_t pulled = bt_.framesPulled();
  stats_.btFramesPerSec =
      static_cast<uint32_t>(static_cast<uint64_t>(pulled - lastBtFrames_) * 1000 / (nowMs - lastStatsMs_));
  lastBtFrames_ = pulled;
  lastStatsMs_ = nowMs;

  const int rate = shared_.rate;
  stats_.bufferedMs = rate > 0 ? static_cast<uint32_t>(static_cast<uint64_t>(ring_->size()) * 1000 / rate) : 0;
  stats_.underruns = shared_.underruns;
  const uint64_t frames = producedFrames_.load();
  const uint64_t busyUs = busyUs_.load();
  stats_.decodeLoad = frames && rate > 0
      ? static_cast<float>(busyUs) / (static_cast<float>(frames) * 1e6f / static_cast<float>(rate))
      : 0.0f;
  stats_.decodeStackFree = uxTaskGetStackHighWaterMark(task_);
}

uint32_t Core2AudioBackend::bufferedMsNow() const {
  const int rate = shared_.rate;
  if (!ring_ || rate <= 0) return 0;
  return static_cast<uint32_t>(static_cast<uint64_t>(ring_->size()) * 1000 / rate);
}

// ---- sharing core 1 with the UI (see the header) ----

void Core2AudioBackend::setRefillPacing(bool enabled, uint32_t capX10) {
  // At least 1.5x (RefillPacer::kMinCapX10): near 1x the paced ring would
  // stop growing at 500 ms.
  paceCapX10_.store(capX10 < RefillPacer::kMinCapX10 ? RefillPacer::kMinCapX10 : capX10, std::memory_order_relaxed);
  paceEnabled_.store(enabled, std::memory_order_relaxed);
}

RefillPacer::Config Core2AudioBackend::refillPacing() const {
  RefillPacer::Config c;
  c.enabled = paceEnabled_.load(std::memory_order_relaxed);
  c.capX10 = paceCapX10_.load(std::memory_order_relaxed);
  return c;
}

Core2AudioBackend::StartTiming Core2AudioBackend::startTiming() const {
  StartTiming t;
  t.seq = startSeq_.load(std::memory_order_acquire);
  t.firstAudioMs = firstAudioMs_.load(std::memory_order_relaxed);
  t.ring500Ms = ring500Ms_.load(std::memory_order_relaxed);
  t.steadyMs = steadyMs_.load(std::memory_order_relaxed);
  t.fullMs = fullMs_.load(std::memory_order_relaxed);
  return t;
}

std::string Core2AudioBackend::description() const {
  std::lock_guard<std::mutex> guard(lock_);
  return description_;
}

std::string Core2AudioBackend::trackTitle() const {
  std::lock_guard<std::mutex> guard(lock_);
  return title_;
}

std::string Core2AudioBackend::trackArtist() const {
  std::lock_guard<std::mutex> guard(lock_);
  return artist_;
}

std::string Core2AudioBackend::note() const {
  std::lock_guard<std::mutex> guard(lock_);
  return note_;
}

void Core2AudioBackend::setText(std::string& field, const std::string& value) {
  std::lock_guard<std::mutex> guard(lock_);
  field = value;
}

// ---- decode task ----

void Core2AudioBackend::taskEntry(void* self) { static_cast<Core2AudioBackend*>(self)->decodeTask(); }

// ID3 tags, on the decode task while a track opens.
void Core2AudioBackend::onMetadata(void* self, const char* type, bool isUnicode, const char* value) {
  // ESP8266Audio hands UTF-16 text over raw, cut short at its first zero byte:
  // keep the file name instead. (Library metadata will come from mStream.)
  if (isUnicode) return;
  auto* b = static_cast<Core2AudioBackend*>(self);
  if (std::strcmp(type, "Title") == 0) b->setText(b->title_, value);
  if (std::strcmp(type, "Performer") == 0) b->setText(b->artist_, value);
}

void Core2AudioBackend::decodeTask() {
  uint32_t generation = sync_.generation();
  Work work = Work::Idle;
  for (;;) {
    const uint32_t latest = sync_.generation();
    if (latest != generation) {
      generation = latest;
      work = start(generation);
      continue;
    }
    switch (work) {
      case Work::Producing:
        switch (toneTrack_ ? produceTone() : produceDecoded()) {
          case Produced::More:
            break;
          case Produced::Done:
            work = Work::Draining;
            ringSteady_ = false;  // the ring empties on purpose now
            shared_.expectingAudio = false;
            sync_.report(generation, Phase::Draining);
            break;
          case Produced::Failed:
            work = fail(generation, std::to_string(out_->rate()) +
                                        " Hz can't play over Bluetooth yet (only 44100 Hz)");
            break;
        }
        break;
      case Work::Draining:
        if (ring_->size() == 0) {
          work = Work::Idle;
          sync_.report(generation, Phase::Ended);
        } else {
          vTaskDelay(pdMS_TO_TICKS(10));
        }
        break;
      case Work::Idle:
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));  // play()/stop() wake us early
        break;
    }
  }
}

Core2AudioBackend::Work Core2AudioBackend::start(uint32_t generation) {
  Request req;
  {
    std::lock_guard<std::mutex> guard(lock_);
    req = request_;
  }
  closeDecoder();
  toneTrack_ = false;
  clickTrack_ = false;
  shared_.expectingAudio = false;
  ringSteady_ = false;  // filling from empty until kSteadyMs
  trackStart_ = ring_->discardAll();  // nothing of the previous track plays after this
  if (req.kind == Kind::Play) {  // a new start to time (startTiming())
    firstAudioMs_ = -1;
    ring500Ms_ = -1;
    steadyMs_ = -1;
    fullMs_ = -1;
    startSeq_.fetch_add(1, std::memory_order_release);
  }
  busyUs_ = 0;
  producedFrames_ = 0;
  srcPos0_ = srcPos_ = srcSize_ = 0;
  knownDurationMs_ = 0;
  // Where it was asked to start, so Now Playing shows that second from the
  // request on; corrected below once the track says where it really lands.
  startMs_ = req.kind == Kind::Play ? req.startMs : 0;
  setText(description_, "");
  setText(title_, "");
  setText(artist_, "");
  if (req.kind == Kind::Stop) return Work::Idle;
  if (req.kind == Kind::Bench) {
    runBench(req.path);
    return Work::Idle;
  }
  // A newly started track plays, unless the player paused since asking. A
  // pause() racing with this counts first and sets paused itself, so checking
  // again after un-pausing leaves it paused whichever store lands last.
  if (pauses_.load() == req.pauses) {
    shared_.paused = false;
    if (pauses_.load() != req.pauses) shared_.paused = true;
  }
  setText(note_, "");

  if (req.path.rfind("tone:", 0) == 0) {
    const std::string what = req.path.substr(5);
    // A built-in track started part of the way in counts from there: the
    // same sound, only what is left of its length (a click track's grid
    // starts again at the start, so the dancer's truth, in track frames
    // from the start, holds).
    auto framesFrom = [&](uint32_t seconds) {
      const uint32_t at = trackseek::startMs(req.startMs, seconds * 1000);
      startMs_ = at;
      if (req.startMs > 0) {
        char asked[12];
        mmss(req.startMs, asked, sizeof(asked));
        Serial.printf("[audio] built-in track: %s asked: %s\n", asked, at ? "counting from there" : "from 0:00");
      }
      return kToneRate * seconds - static_cast<uint32_t>(static_cast<uint64_t>(at) * kToneRate / 1000);
    };
    ClickGen::Spec click;
    if (ClickGen::parse(what, &click)) {
      shared_.rate = kToneRate;
      click_.start(kToneRate, click, framesFrom(kClickSeconds));
      knownDurationMs_ = kClickSeconds * 1000;
      toneTrack_ = clickTrack_ = true;
      char text[48];
      snprintf(text, sizeof(text), "clicks %.0f BPM%s, 44100 Hz", click.bpm, click.offsetBeats > 0 ? ", off-beat start" : "");
      setText(description_, text);
      sync_.report(generation, Phase::Decoding);
      return Work::Producing;
    }
    if (what == "silence") {
      shared_.rate = kToneRate;
      tone_.startSilence(kToneRate, framesFrom(kSilenceSeconds));
      knownDurationMs_ = kSilenceSeconds * 1000;
      toneTrack_ = true;
      setText(description_, "silence (zeros, for power tests), 44100 Hz");
      sync_.report(generation, Phase::Decoding);
      return Work::Producing;
    }
    const bool leftOnly = what == "left";
    const int hz = leftOnly ? 440 : std::atoi(what.c_str());
    if (hz <= 0) return fail(generation, "unknown tone " + req.path);
    shared_.rate = kToneRate;
    tone_.start(kToneRate, static_cast<float>(hz), -18.0f,
                leftOnly ? ToneGen::Channels::LeftOnly : ToneGen::Channels::Both, framesFrom(kToneSeconds));
    knownDurationMs_ = kToneSeconds * 1000;
    toneTrack_ = true;
    setText(description_, "tone " + std::to_string(hz) + " Hz" + (leftOnly ? ", left only" : "") +
                              ", 44100 Hz");
    sync_.report(generation, Phase::Decoding);
    return Work::Producing;
  }

  out_->reset(output_ == Output::Bluetooth);
  if (!openDecoder(req.path, out_.get(), req.startMs, req.hintMs)) return fail(generation, "can't play " + req.path);
  sync_.report(generation, Phase::Decoding);
  return Work::Producing;
}

Core2AudioBackend::Work Core2AudioBackend::fail(uint32_t generation, const std::string& why) {
  Serial.printf("[audio] %s\n", why.c_str());
  closeDecoder();
  setText(note_, why);
  sync_.report(generation, Phase::Failed);
  return Work::Idle;
}

bool Core2AudioBackend::openDecoder(const std::string& path, AudioOutput* out, uint32_t startMs, uint32_t hintMs) {
  const std::string ext = extensionOf(path);
  const bool isMp3 = ext == ".mp3";
  if (!fs_ || (!isMp3 && ext != ".flac")) return false;
  if (!file_->open(path.c_str())) return false;
  const uint32_t size = file_->getSize();
  uint32_t landed = 0;  // where it starts, ms in (startMs_)

  // A fresh generator per track: both keep state across begin() (FLAC its
  // sample buffer, which can replay freed memory; MP3 its last sample). Their
  // big buffers are allocated in begin() anyway, so this costs little.
  AudioGenerator* decoder;
  AudioFileSource* source = file_.get();
  uint32_t flacRate = 0;
  if (isMp3) {  // MP3 reads its title/artist from ID3 tags on the way in
    // A VBR file's length, from the Xing/Info or VBRI header in its first
    // frame (after the ID3v2 tag): the read-rate estimate is only exact for
    // constant bitrates. The same buffer (PSRAM) finds the byte a start part
    // of the way in begins at (mp3StartByte()).
    // ESP8266Audio's ID3 reader goes through the whole tag a byte per read.
    // Through an embedded picture that is seconds of CPU on this task,
    // above the loop: on the device two Moon Safari tracks started 4 s late
    // and froze the UI for as long. A tag this big is skipped instead (its
    // title and artist aren't used: the library has them).
    constexpr uint32_t kMaxTagParsed = 16 * 1024;
    uint32_t start = 0;
    uint32_t from = 0;  // a start part of the way in: the byte the decoder begins at
    auto* probe = static_cast<uint8_t*>(heap_caps_malloc(kMp3Probe, MALLOC_CAP_SPIRAM));
    if (probe) {
      if (file_->read(probe, 10) == 10) start = progress::id3v2Size(probe, 10);
      if (start < size && file_->seek(static_cast<int32_t>(start), SEEK_SET)) {
        const uint32_t got = file_->read(probe, kMp3Probe);
        knownDurationMs_.store(progress::mp3HeaderDurationMs(probe, got), std::memory_order_relaxed);
        if (startMs > 0) from = mp3StartByte(probe, got, start, startMs, hintMs, &landed);
      }
      heap_caps_free(probe);
    }
    if (from > 0) {
      // Past the tags: no ID3 reader (the library has the title and artist).
      file_->seek(static_cast<int32_t>(from), SEEK_SET);
    } else if (probe && start > kMaxTagParsed && start < size) {
      file_->seek(static_cast<int32_t>(start), SEEK_SET);
      Serial.printf("[audio] ID3 tag of %lu KB (a picture?): skipped, not read\n", (unsigned long)(start / 1024));
    } else {
      file_->seek(0, SEEK_SET);
      id3_.reset(new AudioFileSourceID3(file_.get()));
      id3_->RegisterMetadataCB(onMetadata, this);
      source = id3_.get();
    }
    mp3_.reset(new AudioGeneratorMP3());
    decoder = mp3_.get();
  } else {
    // Its length and rate from STREAMINFO (the decoder doesn't expose them
    // before its first frame). Some taggers put an ID3v2 tag in front of
    // "fLaC": libFLAC skips it, and so does this.
    uint8_t head[42];
    if (file_->read(head, sizeof(head)) == sizeof(head)) {
      const uint32_t tag = progress::id3v2Size(head, sizeof(head));
      if (tag > 0 && !(tag < size && file_->seek(static_cast<int32_t>(tag), SEEK_SET) &&
                       file_->read(head, sizeof(head)) == sizeof(head))) {
        std::memset(head, 0, sizeof(head));
      }
      knownDurationMs_.store(progress::flacDurationMs(head, sizeof(head)), std::memory_order_relaxed);
      uint64_t total = 0;
      if (!trackseek::flacStreamInfo(head, sizeof(head), &flacRate, &total)) flacRate = 0;
    }
    file_->seek(0, SEEK_SET);
    flac_.reset(new SeekableFlac());
    decoder = flac_.get();
  }
  if (!decoder->begin(source, out)) {
    closeDecoder();
    return false;
  }
  decoder_ = decoder;
  if (!isMp3 && startMs > 0) {
    // libFLAC's own seek, by sample: exact.
    const uint32_t at = trackseek::startMs(startMs, knownDurationMs_.load(std::memory_order_relaxed));
    char asked[12];
    mmss(startMs, asked, sizeof(asked));
    if (at == 0) {
      Serial.printf("[audio] FLAC: %s asked: in its last %lu s or past its end: from 0:00\n", asked,
                    (unsigned long)(trackseek::kTailMs / 1000));
    } else if (flacRate == 0) {
      // (Nothing is sought: the decoder is as begin() left it.)
      Serial.printf("[audio] FLAC: %s asked: no STREAMINFO found to go by: from 0:00\n", asked);
    } else {
      const int64_t t0 = esp_timer_get_time();
      const bool ok = flac_->seekTo(static_cast<uint64_t>(at) * flacRate / 1000);
      const auto ms = static_cast<unsigned long>((esp_timer_get_time() - t0) / 1000);
      if (!ok) {
        // (Past the end of a file without a length, say.) libFLAC is left
        // in its seek error state: again from the top.
        Serial.printf("[audio] FLAC: %s asked: libFLAC couldn't seek there (%lu ms): from 0:00\n", asked, ms);
        closeDecoder();
        return openDecoder(path, out, 0, 0);
      }
      landed = at;
      Serial.printf("[audio] FLAC: starting %s in (libFLAC's seek, %lu ms)\n", asked, ms);
    }
  }
  codec_ = isMp3 ? "MP3" : "FLAC";
  sourceDone_ = false;
  described_ = false;
  startMs_.store(landed, std::memory_order_relaxed);
  return true;
}

uint32_t Core2AudioBackend::mp3StartByte(uint8_t* probe, uint32_t got, uint32_t audioStart, uint32_t startMs,
                                         uint32_t hintMs, uint32_t* landedMs) {
  const uint32_t size = file_->getSize();
  const uint32_t known = knownDurationMs_.load(std::memory_order_relaxed);
  const uint32_t length = known ? known : trackseek::mp3LengthMs(probe, got, audioStart, size, hintMs);
  const uint32_t at = trackseek::startMs(startMs, length);
  char asked[12], of[12];
  mmss(startMs, asked, sizeof(asked));
  mmss(length, of, sizeof(of));
  if (at == 0) {
    Serial.printf("[audio] MP3: %s asked, of %s: in its last %lu s or past its end: from 0:00\n", asked, of,
                  (unsigned long)(trackseek::kTailMs / 1000));
    return 0;
  }
  uint32_t byte = 0;
  const trackseek::Mp3Seek how = trackseek::mp3SeekByte(probe, got, audioStart, size, hintMs, at, &byte);
  if (how == trackseek::Mp3Seek::Unplaced) {
    Serial.printf("[audio] MP3: %s asked: %s: from 0:00\n", asked, trackseek::mp3SeekName(how));
    return 0;
  }
  if (how == trackseek::Mp3Seek::None || !file_->seek(static_cast<int32_t>(byte), SEEK_SET)) {
    Serial.printf("[audio] MP3: %s asked: no frame found to go on: from 0:00\n", asked);
    return 0;
  }
  // A clean frame from there: libmad would resync by itself, but maybe on
  // a false sync in the audio data first. None near it, or one with the
  // tail or less after it (a file shorter than its header says): from the
  // top, never a start that ends at once (the player would move on).
  const uint32_t n = file_->read(probe, kMp3Probe);
  const int32_t frame = trackseek::mp3FrameAt(probe, n);
  const uint32_t from = byte + static_cast<uint32_t>(frame > 0 ? frame : 0);
  const uint32_t leftMs = frame >= 0 && from < size ? trackseek::mp3MsLeft(probe + frame, size - from) : 0;
  if (leftMs <= trackseek::kTailMs) {
    Serial.printf("[audio] MP3: %s asked, of %s (%s): byte %lu: %s: from 0:00\n", asked, of,
                  trackseek::mp3SeekName(how), (unsigned long)byte,
                  frame < 0 ? "no clean frame near it" : "the file ends right after it");
    return 0;
  }
  *landedMs = at;
  Serial.printf("[audio] MP3: starting %s in, of %s (%s): byte %lu, a frame +%ld\n", asked, of,
                trackseek::mp3SeekName(how), (unsigned long)byte, (long)frame);
  return from;
}

void Core2AudioBackend::closeDecoder() {
  // Always stop(), even when the decoder already says it isn't running: at the
  // end of a file AudioGeneratorFLAC clears `running` itself but only stop()
  // deletes its libFLAC decoder (~100 KB PSRAM + ~2.5 KB internal per track).
  // stop() is safe to repeat for both the MP3 and FLAC generators.
  if (decoder_) decoder_->stop();
  decoder_ = nullptr;
  id3_.reset();
  if (file_) file_->close();
}

Core2AudioBackend::Produced Core2AudioBackend::produceTone() {
  if (ring_->space() < kChunkFrames) {  // ring full: the output is ~1.5 s behind us
    noteStartProgress(kToneRate, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    return Produced::More;
  }
  const int64_t t0 = esp_timer_get_time();
  const uint32_t n = clickTrack_ ? click_.generate(chunk_, kChunkFrames) : tone_.generate(chunk_, kChunkFrames);
  if (n == 0) return Produced::Done;
  ring_->write(chunk_, n);
  const auto toneUs = static_cast<uint64_t>(esp_timer_get_time() - t0);
  busyUs_ += toneUs;
  busyTotalUs_ += toneUs;
  producedFrames_ += n;
  if (!shared_.expectingAudio && producedFrames_ >= kToneRate / 4) shared_.expectingAudio = true;
  noteRingFill(kToneRate);
  noteStartProgress(kToneRate, false);
  vTaskDelay(1);  // share core 1 with the UI loop
  return Produced::More;
}

void Core2AudioBackend::noteStartProgress(int rate, bool full) {
  if (fullMs_.load(std::memory_order_relaxed) >= 0 || rate <= 0) return;  // done for this start
  const auto since = static_cast<int32_t>(millis() - requestMs_.load(std::memory_order_relaxed));
  const uint64_t bufferedMs = static_cast<uint64_t>(ring_->size()) * 1000 / static_cast<uint64_t>(rate);
  if (firstAudioMs_.load(std::memory_order_relaxed) < 0 && producedFrames_ > 0) firstAudioMs_ = since;
  if (ring500Ms_.load(std::memory_order_relaxed) < 0 && bufferedMs >= 500) ring500Ms_ = since;
  if (steadyMs_.load(std::memory_order_relaxed) < 0 && bufferedMs >= kSteadyMs) steadyMs_ = since;
  if (full) fullMs_ = since;
}

void Core2AudioBackend::noteRingFill(int rate) {
  if (ringSteady_.load(std::memory_order_relaxed) || rate <= 0) return;
  if (static_cast<uint64_t>(ring_->size()) * 1000 >= static_cast<uint64_t>(kSteadyMs) * static_cast<uint64_t>(rate)) {
    ringSteady_ = true;
  }
}

Core2AudioBackend::Produced Core2AudioBackend::produceDecoded() {
  if (sourceDone_) {  // end of file: push out the last staged frames, then drain
    if (out_->commit()) {
      closeDecoder();
      return Produced::Done;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    return Produced::More;
  }
  // Room for a full pass plus what RingOutput may already be holding.
  if (ring_->space() < 2 * kChunkFrames) {
    noteStartProgress(out_->rate(), true);
    vTaskDelay(pdMS_TO_TICKS(10));
    return Produced::More;
  }

  const int64_t t0 = esp_timer_get_time();
  const uint64_t before = producedFrames_;
  // Where the file was when the first audio came (the tags in front are
  // left out of the length estimate), and where it is now.
  if (before == 0) srcPos0_.store(file_->getPos(), std::memory_order_relaxed);
  out_->setBudget(kChunkFrames);
  const bool running = decoder_->loop();
  out_->commit();
  srcPos_.store(file_->getPos(), std::memory_order_relaxed);
  srcSize_.store(file_->getSize(), std::memory_order_relaxed);
  const auto passUs = static_cast<uint64_t>(esp_timer_get_time() - t0);
  busyUs_ += passUs;
  busyTotalUs_ += passUs;
  producedFrames_ = before + kChunkFrames - out_->budgetLeft();

  if (out_->rateRejected()) return Produced::Failed;
  if (!described_ && out_->rate() > 0) {
    setText(description_, std::string(codec_) + ", " + std::to_string(out_->rate()) + " Hz");
    described_ = true;
  }
  if (!shared_.expectingAudio && out_->rate() > 0 &&
      producedFrames_ >= static_cast<uint64_t>(out_->rate()) / 4) {
    shared_.expectingAudio = true;  // past the pre-roll
  }
  noteRingFill(out_->rate());
  noteStartProgress(out_->rate(), false);
  if (!running) sourceDone_ = true;
  // Share core 1 with the UI loop: 1 ms, or, with refill pacing on, during
  // the fill after a start or skip (until the ring is first full: fullMs_
  // is -1 until then) and past 500 ms, long enough to keep this track under
  // the rate cap (see setRefillPacing()). Later dips (an SD stall, a
  // Bluetooth burst) refill flat out, as without pacing.
  const int rate = out_->rate();
  RefillPacer::Config pace = refillPacing();
  pace.enabled = pace.enabled && fullMs_.load(std::memory_order_relaxed) < 0;
  vTaskDelay(RefillPacer::sleepMs(pace, bufferedMsNow(), static_cast<uint32_t>(producedFrames_ - before),
                                  rate > 0 ? static_cast<uint32_t>(rate) : 0, static_cast<uint32_t>(passUs)));
  return Produced::More;
}

void Core2AudioBackend::runBench(const std::string& path) {
  CountingOutput counter;
  if (!openDecoder(path, &counter, 0, 0)) {
    Serial.printf("[bench] can't decode %s\n", path.c_str());
    return;
  }
  // Times only the decoding, and yields between bursts so the UI loop runs.
  int64_t busyUs = 0;
  for (;;) {
    counter.budget = CountingOutput::kBurst;
    const int64_t t0 = esp_timer_get_time();
    const bool more = decoder_->loop();
    busyUs += esp_timer_get_time() - t0;
    if (!more) break;
    if (counter.rate > 0 && counter.frames >= static_cast<uint64_t>(counter.rate) * kBenchSeconds) break;
    vTaskDelay(1);
  }
  const double seconds = busyUs / 1e6;
  closeDecoder();
  const double audio = counter.rate > 0 ? static_cast<double>(counter.frames) / counter.rate : 0;
  Serial.printf("[bench] %s: %.1f s of %d Hz audio in %.2f s = %.1fx realtime (%.1f%% of a core), "
                "decode stack free %lu\n",
                path.c_str(), audio, counter.rate, seconds, seconds > 0 ? audio / seconds : 0.0,
                audio > 0 ? 100.0 * seconds / audio : 0.0,
                (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
}
