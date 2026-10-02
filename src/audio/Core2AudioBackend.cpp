// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "audio/Core2AudioBackend.h"

#include <Arduino.h>
#include <AudioFileSourceFS.h>
#include <AudioFileSourceID3.h>
#include <AudioGeneratorFLAC.h>
#include <AudioGeneratorMP3.h>
#include <esp_cpu.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "RateConverter.h"
#include "ResamplerTables.h"
#include "TableCopy.h"
#include "ToneTrack.h"
#include "TrackProgress.h"
#include "TrackSeek.h"
#include "audio/RingOutput.h"

namespace {
constexpr uint32_t kRingFrames = 65536;  // ~1.5 s at 44.1 kHz (every track's rate in the ring), 256 KB of PSRAM
constexpr uint32_t kChunkFrames = 1024;  // source frames taken per pass of the decode task
constexpr uint32_t kDecodeStack = 16384;
constexpr uint32_t kRingRate = AudioShared::kRingRate;
// Bluetooth: what the headphones report plus ESP-IDF's frame queue and the
// air (an estimate); without a report, what the Powerbeats Pro report.
constexpr uint32_t kBtExtraUs = 25000;
constexpr uint32_t kBtDefaultReportUs = 150000;
// The speaker before its first buffer has been timed: ~3 buffers + DMA.
constexpr uint32_t kSpeakerDefaultUs = 115000;
constexpr uint32_t kBenchSeconds = 20;
// The converter's bench (Rb): seconds of audio per rate, and the rates.
constexpr uint32_t kRateBenchSeconds = 10;
constexpr uint32_t kRateBenchRates[] = {48000, 96000, 88200, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 44100};
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
  // be read as 8-bit. (A rate the converter refuses fails the track at its
  // first loop(), as it would from the top.) False: not there; the decoder
  // is then in its seek error state: start again.
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

// The converter's polyphase tables in internal RAM (7.6 KB) while a track
// at another rate than 44.1 kHz plays: copied when one starts, freed when
// a 44.1 kHz one starts (TableCopy; RateConverter calls this from
// setRate(), after the reset, so no track reads a freed copy). Read from
// flash they share the cache with the decoder, and 147/160's 7 KB, read
// every 3.3 ms, evicts it: docs/RESAMPLER.md, section 10. Without the room
// they stay in flash (the same bits, slower), logged once. On the decode
// task.
static void* tableAlloc(size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
static void tableFree(void* p) { heap_caps_free(p); }
static TableCopy tableCopy(tableAlloc, tableFree);
// tableCopy's state for tableStatus() (written on the decode task only).
static std::atomic<bool> tablesInRam{false};
static std::atomic<uint32_t> tableNoRoom{0};

static void tablesWanted(bool wanted) {
  const TableCopy::Event e = tableCopy.want(wanted);
  tablesInRam.store(tableCopy.copied(), std::memory_order_relaxed);
  tableNoRoom.store(tableCopy.failures(), std::memory_order_relaxed);
  const unsigned bytes = (unsigned)TableCopy::kBytes;
  const unsigned freeNow = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  // The largest block too: the next copy needs one 7,776 B block, so a long
  // mixed session's fragmentation shows here before it bites.
  const unsigned largest = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  switch (e) {
    case TableCopy::Event::Copied:
      Serial.printf("[rate] the filter tables copied into internal RAM (%u B): internal free %u B, largest block "
                    "%u B\n",
                    bytes, freeNow, largest);
      break;
    case TableCopy::Event::Freed:
      Serial.printf("[rate] a 44.1 kHz track: the filter tables' copy freed (%u B): internal free %u B, largest "
                    "block %u B\n",
                    bytes, freeNow, largest);
      break;
    case TableCopy::Event::NoRoom:
      Serial.printf("[rate] no %u B block of internal RAM for the filter tables (largest %u B): read from flash, "
                    "the same bits, slower (logged once; each converted track tries again; R counts them)\n",
                    bytes, largest);
      break;
    case TableCopy::Event::StillNoRoom:
    case TableCopy::Event::None:
      break;
  }
}

Core2AudioBackend::TableStatus Core2AudioBackend::tableStatus() {
  return {tablesInRam.load(std::memory_order_relaxed), tableNoRoom.load(std::memory_order_relaxed)};
}

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
  out_.reset(new RingOutput(*ring_));  // under 4 KB: internal RAM (RingOutput.h)
  checkKernel("at boot");  // the converter's fast kernel, only if it gives the C kernel's bits
  RateConverter::setTablesWanted(tablesWanted);
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

void Core2AudioBackend::request(const std::string& path, Kind kind, uint32_t startMs, uint32_t hintMs, uint32_t asHz) {
  if (!task_) return;  // begin() failed
  {
    std::lock_guard<std::mutex> guard(lock_);
    request_ = {path, kind, pauses_.load(), startMs, hintMs, asHz};
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

void Core2AudioBackend::playAsRate(const std::string& path, uint32_t asHz) {
  transportPlaying_ = true;
  request(path, Kind::Play, 0, 0, asHz);
}

void Core2AudioBackend::rateBench() {
  transportPlaying_ = false;
  request("", Kind::RateBench);
}

Core2AudioBackend::RateStatus Core2AudioBackend::rateStatus() const {
  RateStatus r;
  r.rate = convRate_.load(std::memory_order_relaxed);
  r.route = convRoute_.load(std::memory_order_relaxed);
  r.num = convNum_.load(std::memory_order_relaxed);
  r.den = convDen_.load(std::memory_order_relaxed);
  r.taken = convTaken_.load(std::memory_order_relaxed);
  r.made = convMade_.load(std::memory_order_relaxed);
  r.clamped = convClamped_.load(std::memory_order_relaxed);
  return r;
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
      progress::estimateDurationMs(producedFrames_.load(std::memory_order_relaxed), kRingRate,
                                   srcPos0_.load(std::memory_order_relaxed), srcPos_.load(std::memory_order_relaxed),
                                   srcSize_.load(std::memory_order_relaxed));
  return left ? startMs_.load(std::memory_order_relaxed) + left : 0;
}

uint32_t Core2AudioBackend::positionMs() const {
  // Ring frames are 44.1 kHz whatever the track's rate, and ring frame n
  // sits at exactly n / 44100 s of the source (the converter's delay is
  // compensated): exact from the first frame.
  const uint32_t frames = ring_->readPos() - trackStart_;
  const auto played = static_cast<uint32_t>(static_cast<uint64_t>(frames) * 1000 / kRingRate);
  return startMs_.load(std::memory_order_relaxed) + played;
}

bool Core2AudioBackend::positionKnown() const { return sync_.phase() != Phase::Pending; }

bool Core2AudioBackend::finished() const { return sync_.phase() == Phase::Ended; }
bool Core2AudioBackend::failed() const { return sync_.phase() == Phase::Failed; }

IAudioBackend::RateRefusal Core2AudioBackend::rateRefusal() const {
  RateRefusal r;
  if (!failed()) return r;
  r.hz = refusedHz_.load(std::memory_order_relaxed);
  r.needsCpu = r.hz != 0 && refusedForCpu_.load(std::memory_order_relaxed);
  return r;
}

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

  stats_.bufferedMs = bufferedMsNow();
  stats_.underruns = shared_.underruns;
  // Producing (decode and convert) against the audio produced, in ring frames.
  const uint64_t frames = producedFrames_.load();
  const uint64_t busyUs = busyUs_.load();
  stats_.decodeLoad = frames ? static_cast<float>(busyUs) / (static_cast<float>(frames) * 1e6f / kRingRate) : 0.0f;
  stats_.decodeStackFree = uxTaskGetStackHighWaterMark(task_);
}

uint32_t Core2AudioBackend::bufferedMsNow() const {
  if (!ring_) return 0;
  return static_cast<uint32_t>(static_cast<uint64_t>(ring_->size()) * 1000 / kRingRate);
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
            work = failRate(generation);
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
  sourceDone_ = false;
  toneN_ = toneAt_ = 0;
  shared_.expectingAudio = false;
  ringSteady_ = false;  // filling from empty until kSteadyMs
  refusedHz_ = 0;
  trackStart_ = ring_->discardAll();  // nothing of the previous track plays after this
  // ...nor of its converter's history (up to 24 frames), whatever comes
  // next: a file, a built-in track, a stop or a bench.
  feed().reset(cpuMhz());
  publishRate();
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
  if (req.kind == Kind::Bench || req.kind == Kind::RateBench) {
    if (req.kind == Kind::Bench) runBench(req.path);
    if (req.kind == Kind::RateBench) runRateBench();
    feed().reset(cpuMhz());
    publishRate();
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

  if (req.path.rfind("tone:", 0) == 0) return startTone(generation, req);
  feed().forceRate(static_cast<int>(req.asHz));  // (a test: 0 almost always)

  if (!openDecoder(req.path, out_.get(), req.startMs, req.hintMs)) return fail(generation, "can't play " + req.path);
  sync_.report(generation, Phase::Decoding);
  return Work::Producing;
}

Core2AudioBackend::Work Core2AudioBackend::startTone(uint32_t generation, const Request& req) {
  ToneTrack t;
  if (!ToneTrack::parse(req.path, &t)) return fail(generation, "unknown tone " + req.path);
  // Made at its own rate and converted like a file: the test tracks at
  // other rates go through the converter (and its refusals) as a file would.
  if (!feed().setRate(static_cast<int>(t.rate))) return failRate(generation);
  publishRate();
  // A built-in track started part of the way in counts from there: the
  // same sound, only what is left of its length (a click track's grid
  // starts again at the start, so the dancer's truth, in track frames
  // from the start, holds).
  const uint32_t at = trackseek::startMs(req.startMs, t.seconds * 1000);
  startMs_ = at;
  if (req.startMs > 0) {
    char asked[12];
    mmss(req.startMs, asked, sizeof(asked));
    Serial.printf("[audio] built-in track: %s asked: %s\n", asked, at ? "counting from there" : "from 0:00");
  }
  const uint32_t frames = t.rate * t.seconds - static_cast<uint32_t>(static_cast<uint64_t>(at) * t.rate / 1000);
  knownDurationMs_ = t.seconds * 1000;
  toneTrack_ = true;
  char text[64];
  switch (t.kind) {
    case ToneTrack::Kind::Clicks:
      click_.start(t.rate, t.click, frames);
      clickTrack_ = true;
      snprintf(text, sizeof(text), "clicks %.0f BPM%s, %lu Hz", t.click.bpm,
               t.click.offsetBeats > 0 ? ", off-beat start" : "", (unsigned long)t.rate);
      break;
    case ToneTrack::Kind::Silence:
      tone_.startSilence(t.rate, frames);
      snprintf(text, sizeof(text), "silence (zeros, for power tests), %lu Hz", (unsigned long)t.rate);
      break;
    case ToneTrack::Kind::Sine:
    default:
      tone_.start(t.rate, t.hz, -18.0f, t.leftOnly ? ToneGen::Channels::LeftOnly : ToneGen::Channels::Both, frames);
      snprintf(text, sizeof(text), "tone %.0f Hz%s, %lu Hz", t.hz, t.leftOnly ? ", left only" : "",
               (unsigned long)t.rate);
      break;
  }
  setText(description_, text);
  if (t.rate != kRingRate) {
    Serial.printf("[audio] %s: %lu Hz -> %lu Hz (%s)\n", req.path.c_str(), (unsigned long)t.rate,
                  (unsigned long)kRingRate, feed().converter().currentPlan().route);
  }
  sync_.report(generation, Phase::Decoding);
  return Work::Producing;
}

RingFeed& Core2AudioBackend::feed() { return out_->feed(); }

uint32_t Core2AudioBackend::cpuMhz() const {
  const uint16_t set = cpuMhz_.load(std::memory_order_relaxed);
  return set ? set : getCpuFrequencyMhz();
}

std::string Core2AudioBackend::refusalText() {
  return std::to_string(feed().rate()) + " Hz " + feed().refusal();
}

void Core2AudioBackend::publishRate() {
  const RingFeed& f = feed();
  const RateConverter& c = f.converter();
  const RateConverter::Plan& p = c.currentPlan();
  convRate_.store(static_cast<uint32_t>(f.rate()), std::memory_order_relaxed);
  convRoute_.store(c.configured() ? p.route : "", std::memory_order_relaxed);
  convNum_.store(p.num, std::memory_order_relaxed);
  convDen_.store(p.den, std::memory_order_relaxed);
  convTaken_.store(static_cast<uint32_t>(c.taken()), std::memory_order_relaxed);
  convMade_.store(static_cast<uint32_t>(c.produced()), std::memory_order_relaxed);
  convClamped_.store(c.clamped(), std::memory_order_relaxed);
}

Core2AudioBackend::Work Core2AudioBackend::failRate(uint32_t generation) {
  // Stored before fail()'s report, so whoever sees the failure sees why.
  refusedForCpu_.store(feed().refusalKind() == RateConverter::Refusal::NeedsCpu, std::memory_order_relaxed);
  refusedHz_.store(static_cast<uint32_t>(feed().rate()), std::memory_order_relaxed);
  // The console's R reports the refused rate (a built-in track at another
  // rate is refused before any pass publishes it).
  publishRate();
  return fail(generation, refusalText());
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
  if (sourceDone_) return finishSource();
  RingFeed& f = feed();
  if (toneAt_ == toneN_) {  // the last chunk is all in: a new one
    if (ring_->space() < f.roomFor(kChunkFrames)) {  // ring full: the output is ~1.5 s behind us
      noteStartProgress(true);
      vTaskDelay(pdMS_TO_TICKS(10));
      return Produced::More;
    }
    toneN_ = clickTrack_ ? click_.generate(chunk_, kChunkFrames) : tone_.generate(chunk_, kChunkFrames);
    toneAt_ = 0;
    if (toneN_ == 0) {
      sourceDone_ = true;
      return Produced::More;
    }
  }
  // Through RingOutput's converter at the tone's rate. What the ring has no
  // room for stays in chunk_ for the next pass (the generators have moved
  // past it), the way a decoder keeps the sample it couldn't hand over. A
  // pass makes at most kChunkFrames ring frames, as a file's does: an 8 kHz
  // tone's whole chunk would be 5.5 times that.
  const int64_t t0 = esp_timer_get_time();
  const uint64_t before = f.made();
  const uint32_t taken = f.write(chunk_ + 2 * toneAt_, toneN_ - toneAt_, kChunkFrames);
  toneAt_ += taken;
  f.commit();
  const auto toneUs = static_cast<uint64_t>(esp_timer_get_time() - t0);
  busyUs_ += toneUs;
  busyTotalUs_ += toneUs;
  producedFrames_ += f.made() - before;
  publishRate();
  if (!shared_.expectingAudio && producedFrames_ >= kRingRate / 4) shared_.expectingAudio = true;
  noteRingFill();
  noteStartProgress(taken == 0);
  // Share core 1 with the UI loop, paced like a file (produceDecoded()):
  // flat out up to 500 ms of ring, then at most 1.5x realtime until the
  // ring is first full, so a test tone's start can't starve the UI.
  RefillPacer::Config pace = refillPacing();
  pace.enabled = pace.enabled && fullMs_.load(std::memory_order_relaxed) < 0;
  vTaskDelay(taken == 0 ? pdMS_TO_TICKS(10)
                        : RefillPacer::sleepMs(pace, bufferedMsNow(), static_cast<uint32_t>(f.made() - before),
                                               kRingRate, static_cast<uint32_t>(toneUs)));
  return Produced::More;
}

Core2AudioBackend::Produced Core2AudioBackend::finishSource() {
  // The converter's tail (its delay's worth, so the track ends with exactly
  // ceil(source frames x 44100 / rate) ring frames) and the staged frames.
  const uint64_t before = feed().made();
  const bool done = feed().finish();
  producedFrames_ += feed().made() - before;
  publishRate();
  if (done) {
    closeDecoder();
    return Produced::Done;
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  return Produced::More;
}

void Core2AudioBackend::noteStartProgress(bool full) {
  if (fullMs_.load(std::memory_order_relaxed) >= 0) return;  // done for this start
  const auto since = static_cast<int32_t>(millis() - requestMs_.load(std::memory_order_relaxed));
  const uint32_t bufferedMs = bufferedMsNow();
  if (firstAudioMs_.load(std::memory_order_relaxed) < 0 && producedFrames_ > 0) firstAudioMs_ = since;
  if (ring500Ms_.load(std::memory_order_relaxed) < 0 && bufferedMs >= 500) ring500Ms_ = since;
  if (steadyMs_.load(std::memory_order_relaxed) < 0 && bufferedMs >= kSteadyMs) steadyMs_ = since;
  if (full) fullMs_ = since;
}

void Core2AudioBackend::noteRingFill() {
  if (ringSteady_.load(std::memory_order_relaxed)) return;
  if (static_cast<uint64_t>(ring_->size()) * 1000 >= static_cast<uint64_t>(kSteadyMs) * kRingRate) ringSteady_ = true;
}

Core2AudioBackend::Produced Core2AudioBackend::produceDecoded() {
  if (sourceDone_) return finishSource();  // end of file: the converter's tail, the staged frames, then drain
  RingFeed& f = feed();
  // Room for a full pass, in ring frames (at the track's ratio), plus what
  // RingOutput may already be holding.
  if (ring_->space() < f.roomFor(2 * kChunkFrames)) {
    noteStartProgress(true);
    vTaskDelay(pdMS_TO_TICKS(10));
    return Produced::More;
  }

  const int64_t t0 = esp_timer_get_time();
  const uint64_t before = producedFrames_;
  const uint64_t madeBefore = f.made();
  // Where the file was when the first audio came (the tags in front are
  // left out of the length estimate), and where it is now.
  if (before == 0) srcPos0_.store(file_->getPos(), std::memory_order_relaxed);
  // At most kChunkFrames source frames (the decode work per pass as at
  // 44.1 kHz) and kChunkFrames ring frames (the conversion's: an 8 kHz
  // file's 1024 source frames would make 5,645, ~20 ms above the UI loop).
  f.setBudget(kChunkFrames);
  const bool running = decoder_->loop();
  f.commit();
  srcPos_.store(file_->getPos(), std::memory_order_relaxed);
  srcSize_.store(file_->getSize(), std::memory_order_relaxed);
  const auto passUs = static_cast<uint64_t>(esp_timer_get_time() - t0);
  busyUs_ += passUs;
  busyTotalUs_ += passUs;
  producedFrames_ = before + (f.made() - madeBefore);  // ring frames, 44.1 kHz
  publishRate();

  if (f.rejected()) return Produced::Failed;
  if (!described_ && f.rate() > 0) {
    setText(description_, std::string(codec_) + ", " + std::to_string(f.rate()) + " Hz");
    described_ = true;
    if (f.rate() != static_cast<int>(kRingRate)) {
      Serial.printf("[audio] %d Hz -> %lu Hz (%s)\n", f.rate(), (unsigned long)kRingRate,
                    f.converter().currentPlan().route);
    }
  }
  if (!shared_.expectingAudio && producedFrames_ >= kRingRate / 4) {
    shared_.expectingAudio = true;  // past the pre-roll
  }
  noteRingFill();
  noteStartProgress(false);
  if (!running) sourceDone_ = true;
  // Share core 1 with the UI loop: 1 ms, or, with refill pacing on, during
  // the fill after a start or skip (until the ring is first full: fullMs_
  // is -1 until then) and past 500 ms, long enough to keep this track under
  // the rate cap (see setRefillPacing()). Later dips (an SD stall, a
  // Bluetooth burst) refill flat out, as without pacing.
  RefillPacer::Config pace = refillPacing();
  pace.enabled = pace.enabled && fullMs_.load(std::memory_order_relaxed) < 0;
  vTaskDelay(RefillPacer::sleepMs(pace, bufferedMsNow(), static_cast<uint32_t>(producedFrames_ - before), kRingRate,
                                  static_cast<uint32_t>(passUs)));
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
  if (counter.rate <= 0 || counter.rate == static_cast<int>(kRingRate) || audio <= 0) return;

  // Another rate: the same stretch again, decoded and converted through
  // RingOutput (its output dropped), for what the two cost together, cache
  // misses included (Rb times the converter alone).
  RingFeed& f = feed();
  f.reset(cpuMhz(), /*hiRes=*/true);  // measured even while 88.2/96 kHz don't play (RateConverter::kHiResOn)
  if (!openDecoder(path, out_.get(), 0, 0)) return;
  f.setDiscard(true);
  const uint64_t want = counter.frames;
  uint64_t taken = 0;
  busyUs = 0;
  for (;;) {
    f.setBudget(CountingOutput::kBurst);
    const int64_t t0 = esp_timer_get_time();
    const bool more = decoder_->loop();
    f.commit();
    busyUs += esp_timer_get_time() - t0;
    taken += CountingOutput::kBurst - f.budgetLeft();
    if (!more || f.rejected() || taken >= want) break;
    vTaskDelay(1);
  }
  f.setDiscard(false);
  closeDecoder();
  if (f.rejected()) {
    Serial.printf("[bench] %s: not converted: %s\n", path.c_str(), refusalText().c_str());
    return;
  }
  const double both = busyUs / 1e6;
  const double audio2 = static_cast<double>(taken) / counter.rate;
  if (audio2 <= 0) return;
  Serial.printf("[bench] %s: decode + convert to %lu Hz (%s) in %.2f s = %.1f%% of a core at %lu MHz: the converter "
                "%.1f%% in the decoder's company\n",
                path.c_str(), (unsigned long)kRingRate, f.converter().currentPlan().route, both, 100.0 * both / audio2,
                (unsigned long)getCpuFrequencyMhz(), 100.0 * both / audio2 - 100.0 * seconds / audio);
}

bool Core2AudioBackend::checkKernel(const char* when) {
  if (!RateConverter::fastKernelBuilt()) {
    Serial.println("[rate] kernel: C (this build has no fast kernel)");
    return false;
  }
  const uint32_t seed = esp_random();
  const uint32_t c0 = esp_cpu_get_cycle_count();
  const RateConverter::SelfTest t = RateConverter::kernelSelfTest(seed, 4);
  const uint32_t us = (esp_cpu_get_cycle_count() - c0) / getCpuFrequencyMhz();
  if (t.mismatches != 0) kernelFailed_ = true;
  RateConverter::useFastKernel(!kernelFailed_);
  if (t.mismatches == 0) {
    Serial.printf("[rate] kernel: %s; the MAC16 self-test %s gave the C kernel's bits in all %lu dot products "
                  "(seed %08lx, %lu us)\n",
                  kernelFailed_ ? "C (MAC16 failed before: until a restart)" : "MAC16", when, (unsigned long)t.dots,
                  (unsigned long)seed, (unsigned long)us);
  } else {
    Serial.printf("[rate] kernel: MAC16 FAILED its self-test %s: %lu of %lu dot products differ from the C kernel "
                  "(the first: C %ld, MAC16 %ld; seed %08lx). The C kernel from now on, until a restart\n",
                  when, (unsigned long)t.mismatches, (unsigned long)t.dots, (long)t.wantC, (long)t.gotFast,
                  (unsigned long)seed);
  }
  return !kernelFailed_;
}

bool Core2AudioBackend::checkRoutes() {
  if (!RateConverter::fastKernelBuilt()) return true;
  RateConverter& c = feed().benchConverter();
  // Full-scale noise (the clamps too) in chunk_'s first half, the output
  // in its second: blocks of 1-64 frames make at most 64 x 6 + 8 frames.
  constexpr uint32_t kIn = kChunkFrames / 2;
  constexpr uint32_t kFrames = 8192;
  int16_t* out = chunk_ + 2 * kIn;
  uint32_t seed = 4321;
  for (uint32_t i = 0; i < 2 * kIn; ++i) {
    seed = seed * 1664525u + 1013904223u;
    chunk_[i] = static_cast<int16_t>(seed >> 16);
  }
  const bool fast = RateConverter::fastKernel();
  bool same = true;
  uint32_t routes = 0;
  // About 260 M cycles in all (the C kernel at the low rates most of it):
  // the UI loop runs every 5 ms of it, as between the bench's blocks.
  const uint32_t sliceCycles = getCpuFrequencyMhz() * 5000;
  uint32_t sliceStart = esp_cpu_get_cycle_count();
  for (const uint32_t hz : kRateBenchRates) {
    uint32_t hash[2] = {0, 0}, made[2] = {0, 0}, clamped[2] = {0, 0};
    for (int k = 0; k < 2; ++k) {
      RateConverter::useFastKernel(k == 1);
      c.reset();
      if (!c.setRate(hz, RateConverter::kHiResMinMhz, /*hiRes=*/true)) break;
      uint32_t h = 2166136261u, total = 0, blocks = 77;
      auto mix = [&h](const int16_t* p, uint32_t frames) {
        for (uint32_t i = 0; i < 2 * frames; ++i) h = (h ^ static_cast<uint16_t>(p[i])) * 16777619u;
      };
      for (uint32_t done = 0; done < kFrames;) {
        blocks = blocks * 1664525u + 1013904223u;
        const uint32_t n = 1 + (blocks >> 26);  // 1..64
        const uint32_t w = c.convert(chunk_ + 2 * (done % (kIn - 64)), n, out);
        mix(out, w);
        total += w;
        done += n;
        if (esp_cpu_get_cycle_count() - sliceStart >= sliceCycles) {
          vTaskDelay(1);
          sliceStart = esp_cpu_get_cycle_count();
        }
      }
      while (!c.finished()) {
        const uint32_t w = c.finishPush(out);
        mix(out, w);
        total += w;
      }
      hash[k] = h;
      made[k] = total;
      clamped[k] = c.clamped();
    }
    ++routes;
    if (hash[0] != hash[1] || made[0] != made[1] || clamped[0] != clamped[1]) {
      same = false;
      Serial.printf("[rate bench] route check: %lu Hz DIFFERS: C %08lx (%lu frames, %lu clamped), MAC16 %08lx (%lu, %lu)\n",
                    (unsigned long)hz, (unsigned long)hash[0], (unsigned long)made[0], (unsigned long)clamped[0],
                    (unsigned long)hash[1], (unsigned long)made[1], (unsigned long)clamped[1]);
    }
  }
  c.reset();
  if (!same) kernelFailed_ = true;
  RateConverter::useFastKernel(fast && !kernelFailed_);
  Serial.printf("[rate bench] route check: %lu routes, %lu frames each in blocks of 1-64, full-scale noise: MAC16 %s\n",
                (unsigned long)routes, (unsigned long)kFrames,
                same ? "identical to C, bit for bit" : "DIFFERS: the C kernel until a restart");
  return same;
}

void Core2AudioBackend::benchConsume(uint32_t hz) {
  RingFeed& f = feed();
  f.reset(RateConverter::kHiResMinMhz, /*hiRes=*/true);
  f.setDiscard(true);
  if (!f.setRate(static_cast<int>(hz))) return;
  AudioOutput* o = out_.get();  // the generators' virtual call, as they make it
  const uint32_t frames = 2 * hz;  // 2 s of audio
  uint32_t cycles = 0, done = 0, blocks = 0;
  int16_t s[2];
  while (done < frames) {
    f.setBudget(kChunkFrames);
    const uint32_t c0 = esp_cpu_get_cycle_count();
    uint32_t i = 0;
    for (; i < kChunkFrames; ++i) {
      const uint32_t at = 2 * ((done + i) % kChunkFrames);
      s[0] = chunk_[at];
      s[1] = chunk_[at + 1];
      if (!o->ConsumeSample(s)) break;
    }
    f.commit();
    cycles += esp_cpu_get_cycle_count() - c0;
    done += i;
    if (i == 0) break;  // (can't happen: discarding)
    if (++blocks % 8 == 0) vTaskDelay(1);
  }
  f.setDiscard(false);
  Serial.printf("[rate bench] ConsumeSample() a frame at a time at %6lu Hz (%s): %.0f cycles per source frame "
                "(the copy from PSRAM included), %.1f M cycles per second of audio = %.1f%% of a core at %lu MHz\n",
                (unsigned long)hz, f.converter().currentPlan().route, done ? static_cast<double>(cycles) / done : 0.0,
                static_cast<double>(cycles) / 2 / 1e6, 100.0 * cycles / 2 / (getCpuFrequencyMhz() * 1e6),
                (unsigned long)getCpuFrequencyMhz());
}

void Core2AudioBackend::benchKernel() {
  alignas(4) int16_t window[resampler::kTaps + 2];
  int16_t* x = window + 1;  // as the converter's windows: 2 bytes past a 4-byte boundary
  alignas(4) int16_t rowRam[resampler::kTaps];
  for (int j = 0; j < resampler::kTaps; ++j) x[j] = chunk_[j];
  std::memcpy(rowRam, resampler::kD147[37], sizeof(rowRam));
  constexpr uint32_t kDots = 20000;
  struct Case {
    const char* what;
    int32_t (*dot)(const int16_t*, const int16_t*);
    int rows;  // 0: rowRam; 1: one flash row; else walk the stored rows like 147/160
  };
  const Case cases[] = {
      {"C, flash, the table walked", RateConverter::dotC, resampler::kD147Stored},
      {"C, internal RAM row", RateConverter::dotC, 0},
      {"MAC16, flash, the table walked", RateConverter::dotFast, resampler::kD147Stored},
      {"MAC16, flash, one row", RateConverter::dotFast, 1},
      {"MAC16, internal RAM row", RateConverter::dotFast, 0},
  };
  int32_t sink = 0;
  for (const Case& k : cases) {
    if (k.dot == RateConverter::dotFast && !RateConverter::fastKernelBuilt()) continue;
    uint32_t row = 0;
    const uint32_t c0 = esp_cpu_get_cycle_count();
    for (uint32_t i = 0; i < kDots; ++i) {
      const int16_t* c = k.rows == 0 ? rowRam : resampler::kD147[k.rows == 1 ? 37 : row];
      sink += k.dot(c, x);
      row += 13;  // 160 mod 147: the order 147/160 visits its rows in
      if (row >= static_cast<uint32_t>(resampler::kD147Stored)) row -= resampler::kD147Stored;
    }
    const uint32_t cycles = esp_cpu_get_cycle_count() - c0;
    Serial.printf("[rate bench] kernel: %-32s %5.1f cycles per 48-tap dot product (%.2f per multiply, the call "
                  "included)\n",
                  k.what, static_cast<double>(cycles) / kDots, static_cast<double>(cycles) / kDots / resampler::kTaps);
    vTaskDelay(1);
  }
  if (sink == 0x7fffffff) Serial.println("");  // keeps the loops
}

void Core2AudioBackend::runRateBench() {
  RingFeed& f = feed();
  const uint32_t mhz = getCpuFrequencyMhz();
  const uint32_t setting = cpuMhz();
  Serial.printf("[rate bench] %lu s of stereo audio per rate through RingOutput's converter on the decode task, "
                "output dropped; CPU %lu MHz now (set at boot: %lu MHz)\n",
                (unsigned long)kRateBenchSeconds, (unsigned long)mhz, (unsigned long)setting);
  // First the fast kernel against the C one: the dot products, then whole
  // routes. A mismatch turns the fast kernel off.
  checkKernel("in Rb");
  checkRoutes();
  benchKernel();
  const bool fast = RateConverter::fastKernel();
  // A fixed stereo signal (noise at about -12 dBFS, the same every run):
  // the kernel's cost doesn't depend on the values.
  uint32_t seed = 12345;
  for (uint32_t i = 0; i < 2 * kChunkFrames; ++i) {
    seed = seed * 1664525u + 1013904223u;
    chunk_[i] = static_cast<int16_t>(static_cast<int32_t>(seed >> 16) / 4 - 8192);
  }
  for (const uint32_t hz : kRateBenchRates) {
    // Every route is measured, the hi-res ones at 160 MHz too (to settle
    // that rule) and while they don't play (kHiResOn): planned as at 240 MHz.
    // With each kernel: C, then MAC16 when it is on.
    double perSecond[2] = {0, 0};
    for (int k = 0; k < (fast ? 2 : 1); ++k) {
      RateConverter::useFastKernel(k == 1);
      f.reset(RateConverter::kHiResMinMhz, /*hiRes=*/true);
      f.setDiscard(true);
      if (!f.setRate(static_cast<int>(hz))) break;
      uint64_t cycles = 0;
      uint32_t left = hz * kRateBenchSeconds;
      uint32_t blocks = 0;
      while (left > 0) {
        const uint32_t n = left < kChunkFrames ? left : kChunkFrames;
        const uint32_t c0 = esp_cpu_get_cycle_count();
        const uint32_t taken = f.write(chunk_, n);
        f.commit();
        cycles += esp_cpu_get_cycle_count() - c0;  // per block: no 32-bit wrap
        left -= taken;
        if (taken == 0) break;  // (can't happen: discarding, the stage always has room)
        if (++blocks % 4 == 0) vTaskDelay(1);  // the UI loop runs between
      }
      perSecond[k] = static_cast<double>(cycles) / kRateBenchSeconds;
    }
    RateConverter::useFastKernel(fast);
    const RateConverter::Plan p = RateConverter::plan(hz, setting);
    char fastText[48] = "";
    if (fast) {
      snprintf(fastText, sizeof(fastText), "; MAC16 %5.1f M = %4.1f%%", perSecond[1] / 1e6,
               100.0 * perSecond[1] / (mhz * 1e6));
    }
    Serial.printf("[rate bench] %6lu Hz (%s): C %5.1f M cycles per second of audio = %4.1f%%%s of a core at %lu MHz%s%s%s\n",
                  (unsigned long)hz, f.converter().currentPlan().route, perSecond[0] / 1e6,
                  100.0 * perSecond[0] / (mhz * 1e6), fastText, (unsigned long)mhz, p.ok ? "" : " (not played: it ",
                  p.ok ? "" : p.reason, p.ok ? "" : ")");
  }
  f.setDiscard(false);
  // The generators' own path, a frame per call: the passthrough every
  // 44.1 kHz track takes, and the block path at 48 kHz.
  benchConsume(44100);
  benchConsume(48000);
  RateConverter::useFastKernel(fast);
  Serial.printf("[rate bench] cycles include interrupts and anything that preempted the decode task; "
                "b<n> on a file at another rate gives decode + convert together. The kernel now: %s\n",
                RateConverter::fastKernel() ? "MAC16" : "C");
}
