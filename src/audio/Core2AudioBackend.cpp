#include "audio/Core2AudioBackend.h"

#include <Arduino.h>
#include <AudioFileSourceFS.h>
#include <AudioFileSourceID3.h>
#include <AudioGeneratorMP3.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "audio/RingOutput.h"

namespace {
constexpr uint32_t kRingFrames = 65536;  // ~1.5 s at 44.1 kHz, 256 KB of PSRAM
constexpr uint32_t kChunkFrames = 1024;  // produced per pass of the decode task
constexpr uint32_t kDecodeStack = 16384;
constexpr uint32_t kToneRate = 44100;
constexpr uint32_t kToneSeconds = 30;
constexpr uint32_t kBenchSeconds = 20;

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
  mp3_.reset(new AudioGeneratorMP3());

  bt_.begin(*ring_, shared_, btSinkName);
  speaker_.begin(*ring_, shared_);
  setOutput(Output::Speaker);
  setVolume(volume_);

  // Internal-RAM stack: flash reads (LittleFS) can't run on a PSRAM stack.
  xTaskCreatePinnedToCore(taskEntry, "decode", kDecodeStack, this, 2, &task_, APP_CPU_NUM);
  return task_ != nullptr;
}

// ---- control side (loop task) ----

void Core2AudioBackend::request(const std::string& path, Kind kind) {
  {
    std::lock_guard<std::mutex> guard(lock_);
    request_ = {path, kind};
  }
  sync_.post(kind == Kind::Play ? Phase::Pending : Phase::Idle);
  xTaskNotifyGive(task_);
}

bool Core2AudioBackend::play(const std::string& path, uint32_t) {
  shared_.paused = false;  // a newly started track always plays
  request(path, Kind::Play);
  return true;
}

void Core2AudioBackend::stop() { request("", Kind::Stop); }
void Core2AudioBackend::bench(const std::string& path) { request(path, Kind::Bench); }
void Core2AudioBackend::pause() { shared_.paused = true; }
void Core2AudioBackend::resume() { shared_.paused = false; }

bool Core2AudioBackend::isPlaying() const {
  const Phase p = sync_.phase();
  return !shared_.paused &&
         (p == Phase::Pending || p == Phase::Decoding || p == Phase::Draining);
}

uint32_t Core2AudioBackend::positionMs() const {
  const int rate = shared_.rate;
  const uint32_t frames = ring_->readPos() - trackStart_;
  return rate > 0 ? static_cast<uint32_t>(static_cast<uint64_t>(frames) * 1000 / rate) : 0;
}

bool Core2AudioBackend::finished() const { return sync_.phase() == Phase::Ended; }
bool Core2AudioBackend::failed() const { return sync_.phase() == Phase::Failed; }

void Core2AudioBackend::setOutput(Output output) {
  output_ = output;
  ring_->setConsumer(output == Output::Bluetooth ? BtSink::kConsumerId
                                                 : SpeakerSink::kConsumerId);
  Serial.printf("[audio] output: %s\n", output == Output::Bluetooth ? "bluetooth" : "speaker");
}

void Core2AudioBackend::setVolume(uint8_t percent) {
  volume_ = percent > 100 ? 100 : percent;
  speaker_.setVolume(volume_);
  bt_.setVolume(volume_);
}

void Core2AudioBackend::loop(uint32_t nowMs) {
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
  shared_.expectingAudio = false;
  trackStart_ = ring_->discardAll();  // nothing of the previous track plays after this
  busyUs_ = 0;
  producedFrames_ = 0;
  setText(description_, "");
  setText(title_, "");
  setText(artist_, "");
  if (req.kind == Kind::Stop) return Work::Idle;
  if (req.kind == Kind::Bench) {
    runBench(req.path);
    return Work::Idle;
  }
  setText(note_, "");

  if (req.path.rfind("tone:", 0) == 0) {
    const std::string what = req.path.substr(5);
    const bool leftOnly = what == "left";
    const int hz = leftOnly ? 440 : std::atoi(what.c_str());
    if (hz <= 0) return fail(generation, "unknown tone " + req.path);
    shared_.rate = kToneRate;
    tone_.start(kToneRate, static_cast<float>(hz), -18.0f,
                leftOnly ? ToneGen::Channels::LeftOnly : ToneGen::Channels::Both,
                kToneRate * kToneSeconds);
    toneTrack_ = true;
    setText(description_, "tone " + std::to_string(hz) + " Hz" + (leftOnly ? ", left only" : "") +
                              ", 44100 Hz");
    sync_.report(generation, Phase::Decoding);
    return Work::Producing;
  }

  out_->reset(output_ == Output::Bluetooth);
  if (!openDecoder(req.path, out_.get())) return fail(generation, "can't play " + req.path);
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

bool Core2AudioBackend::openDecoder(const std::string& path, AudioOutput* out) {
  if (!fs_ || extensionOf(path) != ".mp3") return false;
  if (!file_->open(path.c_str())) return false;
  id3_.reset(new AudioFileSourceID3(file_.get()));
  id3_->RegisterMetadataCB(onMetadata, this);
  if (!mp3_->begin(id3_.get(), out)) {
    closeDecoder();
    return false;
  }
  decoder_ = mp3_.get();
  sourceDone_ = false;
  described_ = false;
  return true;
}

void Core2AudioBackend::closeDecoder() {
  if (decoder_ && decoder_->isRunning()) decoder_->stop();
  decoder_ = nullptr;
  id3_.reset();
  if (file_) file_->close();
}

Core2AudioBackend::Produced Core2AudioBackend::produceTone() {
  if (ring_->space() < kChunkFrames) {  // ring full: the output is ~1.5 s behind us
    vTaskDelay(pdMS_TO_TICKS(10));
    return Produced::More;
  }
  const int64_t t0 = esp_timer_get_time();
  const uint32_t n = tone_.generate(chunk_, kChunkFrames);
  if (n == 0) return Produced::Done;
  ring_->write(chunk_, n);
  busyUs_ += static_cast<uint64_t>(esp_timer_get_time() - t0);
  producedFrames_ += n;
  if (!shared_.expectingAudio && producedFrames_ >= kToneRate / 4) shared_.expectingAudio = true;
  vTaskDelay(1);  // share core 1 with the UI loop
  return Produced::More;
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
    vTaskDelay(pdMS_TO_TICKS(10));
    return Produced::More;
  }

  const int64_t t0 = esp_timer_get_time();
  const uint64_t before = producedFrames_;
  out_->setBudget(kChunkFrames);
  const bool running = decoder_->loop();
  out_->commit();
  busyUs_ += static_cast<uint64_t>(esp_timer_get_time() - t0);
  producedFrames_ = before + kChunkFrames - out_->budgetLeft();

  if (out_->rateRejected()) return Produced::Failed;
  if (!described_ && out_->rate() > 0) {
    setText(description_, "MP3, " + std::to_string(out_->rate()) + " Hz");
    described_ = true;
  }
  if (!shared_.expectingAudio && out_->rate() > 0 &&
      producedFrames_ >= static_cast<uint64_t>(out_->rate()) / 4) {
    shared_.expectingAudio = true;  // past the pre-roll
  }
  if (!running) sourceDone_ = true;
  vTaskDelay(1);  // share core 1 with the UI loop
  return Produced::More;
}

void Core2AudioBackend::runBench(const std::string& path) {
  CountingOutput counter;
  if (!openDecoder(path, &counter)) {
    Serial.printf("[bench] can't decode %s\n", path.c_str());
    return;
  }
  const int64_t t0 = esp_timer_get_time();
  for (;;) {
    counter.budget = CountingOutput::kBurst;
    if (!decoder_->loop()) break;
    if (counter.rate > 0 && counter.frames >= static_cast<uint64_t>(counter.rate) * kBenchSeconds) break;
  }
  const double seconds = (esp_timer_get_time() - t0) / 1e6;
  closeDecoder();
  const double audio = counter.rate > 0 ? static_cast<double>(counter.frames) / counter.rate : 0;
  Serial.printf("[bench] %s: %.1f s of %d Hz audio in %.2f s = %.1fx realtime (%.1f%% of a core), "
                "decode stack free %lu\n",
                path.c_str(), audio, counter.rate, seconds, seconds > 0 ? audio / seconds : 0.0,
                audio > 0 ? 100.0 * seconds / audio : 0.0,
                (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
}
