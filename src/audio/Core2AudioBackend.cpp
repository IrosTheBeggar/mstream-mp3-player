// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "audio/Core2AudioBackend.h"

#include <Arduino.h>
#include <AudioFileSourceFS.h>
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
#include <new>

#include "RateConverter.h"
#include "ResamplerTables.h"
#include "TableCopy.h"
#include "ToneTrack.h"
#include "TrackProgress.h"
#include "TrackSeek.h"
#include "audio/GuardedSource.h"
#include "audio/PinnedMp3.h"
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
// A chain of gapless joins is under way (decode task): the copy isn't
// freed until the next request's start, even at a join to a 44.1 kHz
// track. A cut rewinds the converter to a state saved earlier in the chain
// (RingFeed::mark()), whose rows may point into the copy: kept, every row
// ever saved stays valid (docs/GAPLESS.md section 5.1).
static bool tablesKeep = false;

static void tablesWanted(bool wanted) {
  if (!wanted && tablesKeep) return;
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

Core2AudioBackend::Core2AudioBackend() : mp3Arena_(PinnedMp3::kArenaParts, 2) {}
Core2AudioBackend::~Core2AudioBackend() = default;

bool Core2AudioBackend::begin(fs::FS* fs, const char* btSinkName) {
  // libmad's state first, while the PSRAM window's fast lower 2 MB is free
  // (docs/RESAMPLER.md section 10d). Without it MP3s decode as before.
  mp3Arena_.attach(heap_caps_aligned_alloc(DecoderArena::kAlign, mp3Arena_.bytes(), MALLOC_CAP_SPIRAM));
  {
    char where[96];
    describeMp3State(where, sizeof(where));
    Serial.printf("[audio] MP3 decoder state: %u B %s\n", (unsigned)mp3Arena_.bytes(), where);
  }
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
  guard_.reset(new GuardedSource());
  // Gapless playback's PSRAM (docs/GAPLESS.md section 6): the trim's hold
  // (16 KB) and two marks of the feed (~2 KB each: a cut's way back). Without
  // them tracks end as before (no hold: no end trim; no marks: no joins).
  holdBuf_ = static_cast<int16_t*>(heap_caps_malloc(TrimFeed::kMaxHold * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  out_->trim().setHoldBuffer(holdBuf_, holdBuf_ ? TrimFeed::kMaxHold : 0);
  for (RingFeed::Mark*& m : marks_) {
    void* mem = heap_caps_malloc(sizeof(RingFeed::Mark), MALLOC_CAP_SPIRAM);
    m = mem ? new (mem) RingFeed::Mark : nullptr;
  }
  engine_.reset(new GaplessEngine(*ring_, feed(), out_->trim(), book_, *this));
  engine_->setMarks(marks_[0], marks_[1]);
  engine_->setEnabled(gapless_.load(std::memory_order_relaxed));

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
  // The heard track's: exact once its file has ended (GaplessJoin's frozen
  // length); before that it is the decoding track, read as before.
  uint32_t exact = 0;
  if (book_.frozenLength(&exact)) return exact;
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
  // compensated): exact from the first frame. The heard track's, from its
  // first frame in the ring; held at its exact end while a join after it
  // waits to be taken (takeAdvance()).
  if (!ring_) return 0;
  return book_.positionMs(sync_.generation(), ring_->readPos());
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
  return heardOverride_ ? heardDescription_ : description_;
}

std::string Core2AudioBackend::note() const {
  std::lock_guard<std::mutex> guard(lock_);
  return note_;
}

void Core2AudioBackend::setText(std::string& field, const std::string& value) {
  std::lock_guard<std::mutex> guard(lock_);
  field = value;
}

// ---- gapless playback: the loop's side ----

void Core2AudioBackend::setNext(const Next& next) {
  book_.setOffer(sync_.generation(), next.after, next.token, next.path, next.hintMs);
  nextAtUs_.store(esp_timer_get_time(), std::memory_order_relaxed);
  if (task_) xTaskNotifyGive(task_);  // a cut at once, not after a 10 ms rest on a full ring
}

bool Core2AudioBackend::takeAdvance(uint32_t* token) {
  const uint32_t readPos = ring_ ? ring_->readPos() : 0;
  if (!ring_ || !book_.takeAdvance(sync_.generation(), readPos, token)) return false;
  {
    // The joined track is the heard one: its description (nothing decodes
    // after it until now), and why it ended early, if it did.
    std::lock_guard<std::mutex> guard(lock_);
    heardOverride_ = false;
    note_ = aheadNote_;
    aheadNote_.clear();
  }
  trackSeq_.fetch_add(1, std::memory_order_release);
  advances_.fetch_add(1, std::memory_order_relaxed);
  Serial.printf("[gapless] heard: the joined track plays (taken %.1f ms after its first frame was read)\n",
                (readPos - book_.status().heardStart) * 1000.0f / kRingRate);
  return true;
}

void Core2AudioBackend::setGapless(bool on) {
  gapless_.store(on, std::memory_order_relaxed);
  if (engine_) engine_->setEnabled(on);
  if (task_) xTaskNotifyGive(task_);
}

bool Core2AudioBackend::durationKnown() const {
  uint32_t exact = 0;
  return book_.frozenLength(&exact) || knownDurationMs_.load(std::memory_order_relaxed) > 0;
}

void Core2AudioBackend::printGapless() const {
  const GaplessJoin::Status st = book_.status();
  const uint32_t r = ring_ ? ring_->readPos() : 0;
  Serial.printf("[gapless] %s; trimming by the LAME tag %s (G0/G1, Gt0/Gt1)\n", gapless() ? "on" : "off (v0.5.0's ends)",
                gaplessTrim() ? "on" : "off");
  if (!st.offer) {
    Serial.println("[gapless] word: none for this request yet");
  } else if (st.offerNow.token == 0) {
    Serial.printf("[gapless] word: nothing follows track %lu\n", (unsigned long)st.offerNow.after);
  } else {
    Serial.printf("[gapless] word: track %lu is followed by %lu, %s%s\n", (unsigned long)st.offerNow.after,
                  (unsigned long)st.offerNow.token, st.offerNow.path.c_str(),
                  st.offerNow.token == st.lastTaken ? " (taken)" : "");
  }
  if (st.boundary) {
    const char* state = st.state == GaplessJoin::State::Pending   ? "cuttable"
                        : st.state == GaplessJoin::State::Cutting ? "being cut"
                                                                  : "too late to cut";
    Serial.printf("[gapless] boundary: track %lu at ring frame %lu (J %lu, %+.0f ms from the reader), %s\n",
                  (unsigned long)st.b.token, (unsigned long)st.b.heardAt, (unsigned long)st.b.cutAt,
                  static_cast<int32_t>(st.b.heardAt - r) * 1000.0f / kRingRate, state);
  } else {
    Serial.println("[gapless] boundary: none");
  }
  if (st.frozen) Serial.printf("[gapless] the heard track's file has ended: exactly %lu ms\n", (unsigned long)st.frozenMs);
  if (engine_) {
    const GaplessEngine::Counters& c = engine_->counters();  // (the decode task's: a snapshot)
    Serial.printf("[gapless] since boot: joins %lu continuous, %lu after the tail (%lu late); heard %lu; cuts %lu, too "
                  "late %lu, retried %lu; opens failed %lu, empty %lu; last cut %lu us after its word (max %lu)\n",
                  (unsigned long)c.joins, (unsigned long)c.resets, (unsigned long)c.late,
                  (unsigned long)advances_.load(std::memory_order_relaxed), (unsigned long)c.cuts,
                  (unsigned long)c.tooLate, (unsigned long)c.retries, (unsigned long)c.failedOpens,
                  (unsigned long)c.emptyAhead, (unsigned long)cutLatencyUs_.load(std::memory_order_relaxed),
                  (unsigned long)cutLatencyMaxUs_.load(std::memory_order_relaxed));
  }
  const uint32_t trim = trimNow_.load(std::memory_order_relaxed);
  if (trim & kTrimLame) {
    Serial.printf("[gapless] decoding track's trim: LAME delay %lu, padding %lu: skipping %lu, holding %lu%s\n",
                  (unsigned long)trimDelay_.load(std::memory_order_relaxed),
                  (unsigned long)trimPadding_.load(std::memory_order_relaxed),
                  (unsigned long)trimSkip_.load(std::memory_order_relaxed),
                  (unsigned long)trimHold_.load(std::memory_order_relaxed),
                  (trim & kTrimCrcBad) ? " (the tag's CRC doesn't match: trimmed anyway)" : "");
  } else if (trim & kTrimMp3) {
    Serial.printf("[gapless] decoding track's trim: no LAME tag: not trimmed (skipping the decoder's lead, %lu)\n",
                  (unsigned long)trimSkip_.load(std::memory_order_relaxed));
  }
}

// ---- decode task ----

void Core2AudioBackend::taskEntry(void* self) { static_cast<Core2AudioBackend*>(self)->decodeTask(); }

void Core2AudioBackend::rest(uint32_t ms) {
  // Not vTaskDelay(): play()/stop()/setNext() wake us (xTaskNotifyGive),
  // so a cut or a new request never waits out a rest.
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
}

void Core2AudioBackend::decodeTask() {
  uint32_t generation = sync_.generation();
  for (;;) {
    const uint32_t latest = sync_.generation();
    if (latest != generation) {
      generation = latest;
      start(generation);
      continue;
    }
    GaplessEngine& e = *engine_;
    const GaplessEngine::Phase p = e.phase();
    if (p == GaplessEngine::Phase::Idle || p == GaplessEngine::Phase::Ended) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));  // play()/stop() wake us early
      continue;
    }
    if (e.cutDue() || p != GaplessEngine::Phase::Producing) {
      // A source's end, the word on what follows, a join, the tail, the
      // drain, a cut: one step at a time, the generation checked between.
      const uint32_t ms = e.step(bufferedMsNow());
      reportPhase(generation);
      if (ms) rest(ms);
      continue;
    }
    switch (toneTrack_ ? produceTone() : produceDecoded()) {
      case Produced::More:
        break;
      case Produced::Done:
        e.sourceEnded(early_);
        break;
      case Produced::Failed:
        failRate(generation);
        break;
    }
  }
}

void Core2AudioBackend::reportPhase(uint32_t generation) {
  Phase want = Phase::Decoding;
  switch (engine_->phase()) {
    case GaplessEngine::Phase::Draining:
      want = Phase::Draining;
      break;
    case GaplessEngine::Phase::Ended:
      want = Phase::Ended;
      break;
    case GaplessEngine::Phase::Idle:
      return;
    default:
      break;
  }
  if (want == reported_) return;
  if (want == Phase::Draining) {
    ringSteady_ = false;  // the ring empties on purpose now
    shared_.expectingAudio = false;
  } else if (want == Phase::Decoding) {
    shared_.expectingAudio = true;  // a late join (or a cut back from a drain): audio from here on
  }
  reported_ = want;
  sync_.report(generation, want);
}

void Core2AudioBackend::start(uint32_t generation) {
  Request req;
  {
    std::lock_guard<std::mutex> guard(lock_);
    req = request_;
  }
  closeDecoder();
  toneTrack_ = false;
  clickTrack_ = false;
  early_ = false;
  toneN_ = toneAt_ = 0;
  shared_.expectingAudio = false;
  ringSteady_ = false;  // filling from empty until kSteadyMs
  refusedHz_ = 0;
  out_->trim().disarm();
  // Nothing of the previous track plays after this, nor of one decoded
  // ahead of it (its boundary goes with the book's restart below)...
  const uint32_t at = ring_->discardAll();
  // ...nor of its converter's history (up to 24 frames), whatever comes
  // next: a file, a built-in track, a stop or a bench. A request is where
  // the tables' copy may go (a 44.1 kHz track); never during a chain of
  // joins (tablesKeep: a cut's rewind may need its rows).
  tablesKeep = false;
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
  {
    std::lock_guard<std::mutex> guard(lock_);
    description_.clear();
    heardOverride_ = false;
    aheadNote_.clear();
  }
  trimNow_ = 0;
  reported_ = Phase::Idle;
  if (req.kind != Kind::Play) {
    engine_->idle(generation);
    if (req.kind == Kind::Bench || req.kind == Kind::RateBench) {
      if (req.kind == Kind::Bench) runBench(req.path);
      if (req.kind == Kind::RateBench) runRateBench();
      feed().reset(cpuMhz());
      publishRate();
    }
    return;
  }
  // A test play at a forced rate (Rf) is never joined: the next track would
  // play at that rate too.
  engine_->begin(generation, at, req.startMs, req.asHz == 0);
  trackSeq_.fetch_add(1, std::memory_order_release);  // (after the book's restart: the new position with it)
  // A newly started track plays, unless the player paused since asking. A
  // pause() racing with this counts first and sets paused itself, so checking
  // again after un-pausing leaves it paused whichever store lands last.
  if (pauses_.load() == req.pauses) {
    shared_.paused = false;
    if (pauses_.load() != req.pauses) shared_.paused = true;
  }
  setText(note_, "");
  feed().forceRate(static_cast<int>(req.asHz));  // (a test: 0 almost always)

  Prepared& p = prepared_;
  if (!prepare(req.path, req.startMs, req.hintMs, &p)) {
    fail(generation, (req.path.rfind("tone:", 0) == 0 ? "unknown tone " : "can't play ") + req.path);
    return;
  }
  if (!beginPrepared(p, out_.get(), true)) {
    if (feed().rejected()) {
      failRate(generation);
    } else {
      fail(generation, "can't play " + req.path);
    }
    return;
  }
  engine_->setStartMs(startMs_.load(std::memory_order_relaxed));
  reported_ = Phase::Decoding;
  sync_.report(generation, Phase::Decoding);
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

void Core2AudioBackend::failRate(uint32_t generation) {
  // Stored before fail()'s report, so whoever sees the failure sees why.
  refusedForCpu_.store(feed().refusalKind() == RateConverter::Refusal::NeedsCpu, std::memory_order_relaxed);
  refusedHz_.store(static_cast<uint32_t>(feed().rate()), std::memory_order_relaxed);
  // The console's R reports the refused rate (a built-in track at another
  // rate is refused before any pass publishes it).
  publishRate();
  fail(generation, refusalText());
}

void Core2AudioBackend::fail(uint32_t generation, const std::string& why) {
  Serial.printf("[audio] %s\n", why.c_str());
  closeDecoder();
  setText(note_, why);
  engine_->idle(generation);
  reported_ = Phase::Failed;
  sync_.report(generation, Phase::Failed);
}

void Core2AudioBackend::endedEarly(const std::string& why) {
  early_ = true;
  Serial.printf("[gapless] %s ended early: %s\n", prepared_.path.c_str(), why.c_str());
  // Its note shows once it is the heard track.
  const bool ahead = book_.status().boundary;
  setText(ahead ? aheadNote_ : note_, why);
}

bool Core2AudioBackend::prepare(const std::string& path, uint32_t startMs, uint32_t hintMs, Prepared* p) {
  *p = Prepared{};
  p->path = path;
  const bool gapless = gapless_.load(std::memory_order_relaxed);
  if (path.rfind("tone:", 0) == 0) {
    // A built-in track (lib/core ToneTrack): made at its own rate and
    // converted like a file, so the test tracks at other rates go through
    // the converter (and its refusals) as a file would. Sample-exact: no
    // trim. Part of the way in it counts from there: the same sound, only
    // what is left of its length (a click track's grid starts again at the
    // start, so the dancer's truth, in track frames from the start, holds).
    if (!ToneTrack::parse(path, &p->toneSpec)) return false;
    p->tone = true;
    p->rate = p->toneSpec.rate;
    p->knownMs = p->toneSpec.seconds * 1000;
    p->landedMs = trackseek::startMs(startMs, p->knownMs);
    if (startMs > 0) {
      char asked[12];
      mmss(startMs, asked, sizeof(asked));
      Serial.printf("[audio] built-in track: %s asked: %s\n", asked, p->landedMs ? "counting from there" : "from 0:00");
    }
    return true;
  }
  const std::string ext = extensionOf(path);
  p->mp3 = ext == ".mp3";
  if (!fs_ || (!p->mp3 && ext != ".flac")) return false;
  if (!file_->open(path.c_str())) return false;
  const uint32_t size = file_->getSize();

  if (p->mp3) {
    // Past the ID3v2 tags (one after another, as some taggers leave them)
    // to the first frame: no ID3 reader (the library has the title and the
    // artist; ESP8266Audio's reads a tag a byte at a time, above the UI,
    // and at a join that would be while the track before plays).
    uint32_t start = 0;
    for (int i = 0; i < 4; ++i) {
      uint8_t head[10];
      if (!file_->seek(static_cast<int32_t>(start), SEEK_SET) || file_->read(head, sizeof(head)) != sizeof(head)) break;
      const uint32_t tag = progress::id3v2Size(head, sizeof(head));
      if (tag == 0 || start + tag >= size) break;
      start += tag;
    }
    if (start > 16 * 1024) {
      Serial.printf("[audio] ID3 tag of %lu KB (a picture?): skipped, not read\n", (unsigned long)(start / 1024));
    }
    p->from = start;
    // The first frame (PSRAM): its Xing/Info or VBRI header (the length,
    // trimmed by LAME's tag; a frame with no audio, not decoded), its rate
    // (a join's continuity is decided before any frame), and the byte a
    // start part of the way in begins at (mp3StartByte()).
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(kMp3Probe, MALLOC_CAP_SPIRAM));
    if (buf) {
      if (start < size && file_->seek(static_cast<int32_t>(start), SEEK_SET)) {
        const uint32_t got = file_->read(buf, kMp3Probe);
        lametag::parse(buf, got, &p->lame);
        p->knownMs = progress::mp3HeaderDurationMs(buf, got);
        p->rate = p->lame.frame ? p->lame.rate : 0;
        if (gapless && p->lame.header) p->from = start + p->lame.frameAt + p->lame.headerLength;
        if (startMs > 0) {
          const uint32_t seekFrom = mp3StartByte(buf, got, start, startMs, hintMs, p->knownMs, &p->landedMs);
          if (seekFrom > 0) {
            p->from = seekFrom;
            p->fromTop = false;
          }
        }
      }
      heap_caps_free(buf);
    }
    // The trim: the generator's lead always (its constructor's {0,0}, not
    // in the file), LAME's delay + 529 from the top and its padding - 529
    // at the end (Gt0: neither). G0: nothing, as v0.5.0.
    if (gapless) p->trim = lametag::trim(p->lame, 1, p->fromTop, gaplessTrim_.load(std::memory_order_relaxed));
    p->guard = gapless;
    return true;
  }

  // FLAC: its length and rate from STREAMINFO (the decoder doesn't expose
  // them before its first frame). Some taggers put an ID3v2 tag in front of
  // "fLaC": libFLAC skips it, and so does this.
  uint8_t head[42];
  uint32_t tag = 0;
  if (file_->read(head, sizeof(head)) == sizeof(head)) {
    tag = progress::id3v2Size(head, sizeof(head));
    if (tag > 0 && !(tag < size && file_->seek(static_cast<int32_t>(tag), SEEK_SET) &&
                     file_->read(head, sizeof(head)) == sizeof(head))) {
      std::memset(head, 0, sizeof(head));
    }
    p->knownMs = progress::flacDurationMs(head, sizeof(head));
    uint64_t total = 0;
    if (!trackseek::flacStreamInfo(head, sizeof(head), &p->rate, &total)) p->rate = 0;
  }
  // Its metadata blocks: libFLAC reads through the ones it doesn't keep (an
  // embedded picture: megabytes) before the first frame, so at a join a big
  // one must fit in what the ring holds (logged; docs/GAPLESS.md section 12).
  if (p->rate) {
    uint32_t at = tag + 4;
    for (int i = 0; i < 64 && at + 4 <= size; ++i) {
      uint8_t h[4];
      if (!file_->seek(static_cast<int32_t>(at), SEEK_SET) || file_->read(h, sizeof(h)) != sizeof(h)) break;
      const uint32_t len = (static_cast<uint32_t>(h[1]) << 16) | (static_cast<uint32_t>(h[2]) << 8) | h[3];
      p->metadataBytes += 4 + len;
      at += 4 + len;
      if (h[0] & 0x80) break;  // the last block
    }
  }
  if (startMs > 0) {
    // libFLAC's own seek, by sample (exact), after begin().
    const uint32_t landed = trackseek::startMs(startMs, p->knownMs);
    char asked[12];
    mmss(startMs, asked, sizeof(asked));
    if (landed == 0) {
      Serial.printf("[audio] FLAC: %s asked: in its last %lu s or past its end: from 0:00\n", asked,
                    (unsigned long)(trackseek::kTailMs / 1000));
    } else if (p->rate == 0) {
      Serial.printf("[audio] FLAC: %s asked: no STREAMINFO found to go by: from 0:00\n", asked);
    } else {
      p->flacSeekMs = landed;
      p->landedMs = landed;  // positionMs() and the resume point count from there
      p->fromTop = false;
    }
  }
  // The trim: after a seek the generator's lead ({0,0}, once it knows its
  // channels); from the top there is none. FLAC is sample-exact.
  if (gapless && !p->fromTop) p->trim.skip = 1;
  return true;
}

bool Core2AudioBackend::beginPrepared(const Prepared& p, AudioOutput* out, bool trimmed) {
  if (trimmed) {
    out_->trim().arm(p.trim.skip, p.trim.hold);
  } else {
    out_->trim().disarm();
  }
  busyUs_ = 0;
  producedFrames_ = 0;
  srcPos0_ = srcPos_ = srcSize_ = 0;
  described_ = false;
  early_ = false;
  toneTrack_ = false;
  clickTrack_ = false;
  toneN_ = toneAt_ = 0;
  knownDurationMs_.store(p.knownMs, std::memory_order_relaxed);
  startMs_.store(p.landedMs, std::memory_order_relaxed);
  if (trimmed) {
    uint32_t trim = 0;
    if (p.mp3) trim |= kTrimMp3;
    if (p.mp3 && p.lame.lame && p.trim.hold + p.trim.skip > 1) trim |= kTrimLame;
    if (p.lame.crcChecked && !p.lame.crcOk) trim |= kTrimCrcBad;
    trimDelay_.store(p.lame.delay, std::memory_order_relaxed);
    trimPadding_.store(p.lame.padding, std::memory_order_relaxed);
    trimSkip_.store(p.trim.skip, std::memory_order_relaxed);
    trimHold_.store(p.trim.hold, std::memory_order_relaxed);
    trimNow_.store(trim, std::memory_order_relaxed);
  }

  if (p.tone) {
    const ToneTrack& t = p.toneSpec;
    if (!feed().setRate(static_cast<int>(t.rate))) return false;
    publishRate();
    const uint32_t frames =
        t.rate * t.seconds - static_cast<uint32_t>(static_cast<uint64_t>(p.landedMs) * t.rate / 1000);
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
      Serial.printf("[audio] %s: %lu Hz -> %lu Hz (%s)\n", p.path.c_str(), (unsigned long)t.rate,
                    (unsigned long)kRingRate, feed().converter().currentPlan().route);
    }
    return true;
  }

  // A fresh generator per track: both keep state across begin() (FLAC its
  // sample buffer, which can replay freed memory; MP3 its last sample). Their
  // big buffers are allocated in begin() anyway, so this costs little.
  AudioGenerator* decoder;
  AudioFileSource* source = file_.get();
  if (p.mp3) {
    if (!file_->seek(static_cast<int32_t>(p.from), SEEK_SET)) {
      closeDecoder();
      return false;
    }
    if (p.guard) {
      guard_->attach(file_.get());
      source = guard_.get();
    }
    mp3_.reset(makeMp3());
    decoder = mp3_.get();
    if (trimmed && gapless_.load(std::memory_order_relaxed)) {
      if (p.lame.lame && gaplessTrim_.load(std::memory_order_relaxed)) {
        Serial.printf("[gapless] trim: %s delay %u, padding %u: skipping %lu, holding %lu%s\n", p.lame.encoder,
                      (unsigned)p.lame.delay, (unsigned)p.lame.padding, (unsigned long)p.trim.skip,
                      (unsigned long)p.trim.hold, p.lame.crcChecked && !p.lame.crcOk ? " (tag CRC mismatch)" : "");
      } else if (!p.lame.lame) {
        Serial.printf("[gapless] %s: no LAME tag: not trimmed\n", p.lame.header ? "a header frame (skipped)" : "no header");
      }
    }
  } else {
    file_->seek(0, SEEK_SET);
    flac_.reset(new SeekableFlac());
    decoder = flac_.get();
  }
  if (!decoder->begin(source, out)) {
    closeDecoder();
    return false;
  }
  decoder_ = decoder;
  codec_ = p.mp3 ? "MP3" : "FLAC";
  if (!p.mp3 && p.metadataBytes > 256 * 1024) {
    Serial.printf("[audio] FLAC: %lu KB of metadata (a picture?) read through before its first frame\n",
                  (unsigned long)(p.metadataBytes / 1024));
  }
  if (p.flacSeekMs > 0) {
    char asked[12];
    mmss(p.flacSeekMs, asked, sizeof(asked));
    const int64_t t0 = esp_timer_get_time();
    const bool ok = flac_->seekTo(static_cast<uint64_t>(p.flacSeekMs) * p.rate / 1000);
    const auto ms = static_cast<unsigned long>((esp_timer_get_time() - t0) / 1000);
    if (!ok) {
      // (Past the end of a file without a length, say.) libFLAC is left
      // in its seek error state: again from the top.
      Serial.printf("[audio] FLAC: %s asked: libFLAC couldn't seek there (%lu ms): from 0:00\n", asked, ms);
      closeDecoder();
      Prepared top;
      return prepare(p.path, 0, 0, &top) && beginPrepared(top, out, trimmed);
    }
    Serial.printf("[audio] FLAC: starting %s in (libFLAC's seek, %lu ms)\n", asked, ms);
  }
  return true;
}

bool Core2AudioBackend::openDecoder(const std::string& path, AudioOutput* out, uint32_t startMs, uint32_t hintMs,
                                    bool trimmed) {
  Prepared& p = prepared_;
  return prepare(path, startMs, hintMs, &p) && !p.tone && beginPrepared(p, out, trimmed);
}

uint32_t Core2AudioBackend::mp3StartByte(uint8_t* probe, uint32_t got, uint32_t audioStart, uint32_t startMs,
                                         uint32_t hintMs, uint32_t known, uint32_t* landedMs) {
  const uint32_t size = file_->getSize();
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

AudioGeneratorMP3* Core2AudioBackend::makeMp3() {
  mp3_.reset();  // the track before's generator gives the block back first
  mp3Pinned_ = true;
  if (AudioGeneratorMP3* g = PinnedMp3::make(mp3Arena_)) return g;
  mp3Pinned_ = false;
  ++mp3Unpinned_;
  Serial.printf("[audio] MP3: libmad's state malloc'd for this track (%s; %lu so far): its speed depends on where "
                "it lands\n",
                !mp3Arena_.attached() ? "no pinned block" : mp3Arena_.inUse() ? "the pinned block is in use" : "no RAM",
                (unsigned long)mp3Unpinned_);
  return new AudioGeneratorMP3();
}

void Core2AudioBackend::describeMp3State(char* buf, size_t size) const {
  if (!mp3Arena_.attached()) {
    snprintf(buf, size, "not pinned (no PSRAM block): malloc'd per track");
    return;
  }
  const void* b = mp3Arena_.block();
  snprintf(buf, size, "pinned at %p, %s", b,
           DecoderArena::whereName(DecoderArena::where(b, mp3Arena_.bytes())));
}

void Core2AudioBackend::closeDecoder() {
  // Always stop(), even when the decoder already says it isn't running: at the
  // end of a file AudioGeneratorFLAC clears `running` itself but only stop()
  // deletes its libFLAC decoder (~100 KB PSRAM + ~2.5 KB internal per track).
  // stop() is safe to repeat for both the MP3 and FLAC generators.
  if (decoder_) decoder_->stop();
  decoder_ = nullptr;
  toneTrack_ = false;
  clickTrack_ = false;
  if (file_) file_->close();
}

// ---- gapless playback: the decode task's side (GaplessEngine::Tracks) ----

bool Core2AudioBackend::probe(const GaplessJoin::Offer& next, uint32_t* rate) {
  probeUs_ = esp_timer_get_time();
  if (!prepare(next.path, 0, next.hintMs, &prepared_)) return false;
  *rate = prepared_.rate;
  return true;
}

bool Core2AudioBackend::start() {
  // The heard track's description stays until the join is heard.
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (!heardOverride_) {
      heardDescription_ = description_;
      heardOverride_ = true;
    }
  }
  tablesKeep = true;  // a chain of joins: the tables' copy stays until the next request
  if (!beginPrepared(prepared_, out_.get(), true)) return false;
  openMs_ = static_cast<uint32_t>((esp_timer_get_time() - probeUs_) / 1000);
  return true;
}

void Core2AudioBackend::close() { closeDecoder(); }

void Core2AudioBackend::note(const GaplessEngine::Note& n) {
  const float ms = static_cast<int32_t>(n.ringFrames) * 1000.0f / kRingRate;
  switch (n.event) {
    case GaplessEngine::Event::Joined:
    case GaplessEngine::Event::JoinedReset:
      Serial.printf("[gapless] decoding ahead: %s (%lu Hz, %s) with %.0f ms of the track before left; opened in %lu "
                    "ms\n",
                    prepared_.path.c_str(), (unsigned long)n.rate,
                    n.event == GaplessEngine::Event::Joined ? "the same rate: one stream"
                    : n.late                                ? "late: after the tail"
                                                            : "another rate: after the tail",
                    ms, (unsigned long)openMs_);
      if (prepared_.metadataBytes > 1024 * 1024) {
        Serial.printf("[gapless] (its %lu KB of metadata were read through at the open: an underrun risk)\n",
                      (unsigned long)(prepared_.metadataBytes / 1024));
      }
      break;
    case GaplessEngine::Event::OpenFailed:
      Serial.printf("[gapless] can't decode ahead %s; the track before ends as before\n", prepared_.path.c_str());
      break;
    case GaplessEngine::Event::EmptyAhead:
      Serial.printf("[gapless] %s gave no audio: taken back out\n", prepared_.path.c_str());
      break;
    case GaplessEngine::Event::Cut: {
      const int64_t asked = nextAtUs_.load(std::memory_order_relaxed);
      const uint32_t us = asked > 0 ? static_cast<uint32_t>(esp_timer_get_time() - asked) : 0;
      cutLatencyUs_.store(us, std::memory_order_relaxed);
      if (us > cutLatencyMaxUs_.load(std::memory_order_relaxed)) cutLatencyMaxUs_.store(us, std::memory_order_relaxed);
      Serial.printf("[gapless] cut: what comes next changed: the track decoded ahead taken back out, %.0f ms before "
                    "the join (%lu us after the word)\n",
                    ms, (unsigned long)us);
      break;
    }
    case GaplessEngine::Event::CutTooLate:
      Serial.println("[gapless] too late to cut: the outputs are past the join; the change applies once it is heard");
      break;
  }
}

Core2AudioBackend::Produced Core2AudioBackend::produceTone() {
  RingFeed& f = feed();
  if (toneAt_ == toneN_) {  // the last chunk is all in: a new one
    if (ring_->space() < f.roomFor(kChunkFrames)) {  // ring full: the output is ~1.5 s behind us
      noteStartProgress(true);
      rest(10);
      return Produced::More;
    }
    toneN_ = clickTrack_ ? click_.generate(chunk_, kChunkFrames) : tone_.generate(chunk_, kChunkFrames);
    toneAt_ = 0;
    if (toneN_ == 0) return Produced::Done;  // its end
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
  rest(taken == 0 ? 10
                  : RefillPacer::sleepMs(pace, bufferedMsNow(), static_cast<uint32_t>(f.made() - before), kRingRate,
                                         static_cast<uint32_t>(toneUs)));
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
  RingFeed& f = feed();
  // Room for a full pass, in ring frames (at the track's ratio), plus what
  // RingOutput may already be holding.
  if (ring_->space() < f.roomFor(2 * kChunkFrames)) {
    noteStartProgress(true);
    rest(10);
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

  if (f.rejected()) {
    // The request's own track refused before its first frame: it fails, as
    // before. A track joined, or one that has played: an early end (what
    // it gave plays; docs/GAPLESS.md section 5.3).
    if (engine_->decodingToken() == 0 && producedFrames_ == 0) return Produced::Failed;
    endedEarly(refusalText());
    return Produced::Done;
  }
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
  if (!running) {
    // Its end. Before the file's (a decode error the generator gave up on:
    // libmad's, which closes the file too): what the trim holds is real
    // audio, not the padding.
    early_ = !file_->isOpen() || file_->getPos() < file_->getSize();
    return Produced::Done;
  }
  // Share core 1 with the UI loop: 1 ms, or, with refill pacing on, during
  // the fill after a start or skip (until the ring is first full: fullMs_
  // is -1 until then) and past 500 ms, long enough to keep this track under
  // the rate cap (see setRefillPacing()). Later dips (an SD stall, a
  // Bluetooth burst) refill flat out, as without pacing.
  RefillPacer::Config pace = refillPacing();
  pace.enabled = pace.enabled && fullMs_.load(std::memory_order_relaxed) < 0;
  rest(RefillPacer::sleepMs(pace, bufferedMsNow(), static_cast<uint32_t>(producedFrames_ - before), kRingRate,
                            static_cast<uint32_t>(passUs)));
  return Produced::More;
}

void Core2AudioBackend::runBench(const std::string& path) {
  CountingOutput counter;
  if (!openDecoder(path, &counter, 0, 0, false)) {
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
  if (std::strcmp(codec_, "MP3") == 0) {
    // Its speed depends on where libmad's state is (RESAMPLER.md section 10d).
    char where[96];
    describeMp3State(where, sizeof(where));
    if (!mp3Pinned_) snprintf(where, sizeof(where), "malloc'd for this track, not pinned");
    Serial.printf("[bench] libmad's state: %s\n", where);
  }
  if (counter.rate <= 0 || counter.rate == static_cast<int>(kRingRate) || audio <= 0) return;

  // Another rate: the same stretch again, decoded and converted through
  // RingOutput (its output dropped), for what the two cost together, cache
  // misses included (Rb times the converter alone).
  RingFeed& f = feed();
  f.reset(cpuMhz(), /*hiRes=*/true);  // measured even while 88.2/96 kHz don't play (RateConverter::kHiResOn)
  if (!openDecoder(path, out_.get(), 0, 0, false)) return;
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
